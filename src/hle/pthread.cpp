// bionic pthreads + semaphores + sleeping. Guest sync objects are 4-byte words; the real
// state lives host side, keyed by the object's guest address.
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "hle/hle.h"
#include "hle/timing.h"

namespace hle {

namespace {

using Clock = std::chrono::steady_clock;

struct GMutex {
    std::mutex m;
    std::condition_variable cv;
    u32 owner = 0;
    int count = 0;
    int type = 0;  // 0 normal, 1 recursive, 2 errorcheck

    int lock(u32 self, bool try_only) {
        std::unique_lock lk(m);
        if (owner == self) {
            if (type == 2) return 35;  // EDEADLK
            count++;                   // treat normal re-lock as recursive instead of hanging
            return 0;
        }
        if (owner) {
            if (try_only) return G_EBUSY;
            cv.wait(lk, [&] { return owner == 0; });
        }
        owner = self;
        count = 1;
        return 0;
    }
    int unlock(u32 self) {
        std::lock_guard lk(m);
        if (owner != self) return G_EPERM;
        if (--count == 0) {
            owner = 0;
            cv.notify_one();
        }
        return 0;
    }
    int release_all(u32 self) {
        std::lock_guard lk(m);
        if (owner != self) return 0;
        int c = count;
        owner = 0;
        count = 0;
        cv.notify_one();
        return c;
    }
    void reacquire(u32 self, int c) {
        std::unique_lock lk(m);
        cv.wait(lk, [&] { return owner == 0; });
        owner = self;
        count = c ? c : 1;
    }
};

struct GCond {
    std::mutex m;
    std::condition_variable cv;
    int waiters = 0;
    int wakeups = 0;
};

struct GSem {
    std::mutex m;
    std::condition_variable cv;
    u32 count = 0;
};

std::mutex g_obj_lock;
std::unordered_map<u32, GMutex*> g_mutexes;
std::unordered_map<u32, GCond*> g_conds;
std::unordered_map<u32, GSem*> g_sems;

GMutex* get_mutex(u32 a) {
    std::lock_guard lk(g_obj_lock);
    auto& p = g_mutexes[a];
    if (!p) {
        p = new GMutex;
        u32 v = mem::r32(a);
        p->type = (v & 0xC000) == 0x4000 ? 1 : (v & 0xC000) == 0x8000 ? 2 : 0;
    }
    return p;
}

GCond* get_cond(u32 a) {
    std::lock_guard lk(g_obj_lock);
    auto& p = g_conds[a];
    if (!p) p = new GCond;
    return p;
}

GSem* get_sem(u32 a) {
    std::lock_guard lk(g_obj_lock);
    auto& p = g_sems[a];
    if (!p) {
        p = new GSem;
        p->count = mem::r32(a);
    }
    return p;
}

// guest timespec holding an absolute CLOCK_REALTIME deadline
Clock::time_point abs_time(u32 ts) {
    s64 sec = s32(mem::r32(ts));
    s64 nsec = s32(mem::r32(ts + 4));
    return timing::from_realtime(sec * 1000000000 + nsec);
}

int cond_wait(u32 cond, u32 mutex, const Clock::time_point* deadline) {
    u32 self = cpu::thread().tid;
    GCond* c = get_cond(cond);
    GMutex* m = get_mutex(mutex);
    int saved;
    bool timed_out = false;
    {
        std::unique_lock lk(c->m);
        c->waiters++;
        saved = m->release_all(self);
        while (c->wakeups == 0) {
            if (deadline) {
                if (c->cv.wait_until(lk, *deadline) == std::cv_status::timeout && c->wakeups == 0) {
                    timed_out = true;
                    break;
                }
            } else {
                c->cv.wait(lk);
            }
        }
        if (!timed_out) c->wakeups--;
        c->waiters--;
    }
    m->reacquire(self, saved);
    return timed_out ? G_ETIMEDOUT : 0;
}

// ---------------------------------------------------------------- threads
struct ThreadRec {
    u32 stack_base, stack_size;
};
std::mutex g_threads_lock;
std::map<u32, ThreadRec> g_threads;
std::atomic<u32> g_next_thread_id{2000};

std::mutex g_key_lock;
u32 g_next_key = 1;
thread_local std::unordered_map<u32, u32> t_specific;

std::recursive_mutex g_once_lock;

}  // namespace

void register_pthread() {
    // ---------------------------------------------------------- mutex
    reg("pthread_mutexattr_init", [](Ctx& c) { mem::w32(c.arg(0), 0); c.ret(0); });
    reg("pthread_mutexattr_destroy", [](Ctx& c) { c.ret(0); });
    reg("pthread_mutexattr_settype", [](Ctx& c) { mem::w32(c.arg(0), c.arg(1)); c.ret(0); });
    reg("pthread_mutexattr_setpshared", [](Ctx& c) { c.ret(0); });
    reg("pthread_mutex_init", [](Ctx& c) {
        u32 a = c.arg(0);
        int type = c.arg(1) ? int(mem::r32(c.arg(1))) : 0;
        mem::w32(a, type == 1 ? 0x4000 : type == 2 ? 0x8000 : 0);
        std::lock_guard lk(g_obj_lock);
        auto& p = g_mutexes[a];
        delete p;
        p = new GMutex;
        p->type = type;
        c.ret(0);
    });
    reg("pthread_mutex_destroy", [](Ctx& c) {
        std::lock_guard lk(g_obj_lock);
        auto it = g_mutexes.find(c.arg(0));
        if (it != g_mutexes.end()) {
            delete it->second;
            g_mutexes.erase(it);
        }
        c.ret(0);
    });
    reg("pthread_mutex_lock", [](Ctx& c) { c.ret(u32(get_mutex(c.arg(0))->lock(cpu::thread().tid, false))); });
    reg("pthread_mutex_trylock", [](Ctx& c) { c.ret(u32(get_mutex(c.arg(0))->lock(cpu::thread().tid, true))); });
    reg("pthread_mutex_unlock", [](Ctx& c) { c.ret(u32(get_mutex(c.arg(0))->unlock(cpu::thread().tid))); });

    // ---------------------------------------------------------- condition variables
    reg("pthread_cond_init", [](Ctx& c) {
        mem::w32(c.arg(0), 0);
        std::lock_guard lk(g_obj_lock);
        auto& p = g_conds[c.arg(0)];
        delete p;
        p = new GCond;
        c.ret(0);
    });
    reg("pthread_cond_destroy", [](Ctx& c) {
        std::lock_guard lk(g_obj_lock);
        auto it = g_conds.find(c.arg(0));
        if (it != g_conds.end()) {
            delete it->second;
            g_conds.erase(it);
        }
        c.ret(0);
    });
    reg("pthread_cond_signal", [](Ctx& c) {
        GCond* cv = get_cond(c.arg(0));
        std::lock_guard lk(cv->m);
        if (cv->waiters > cv->wakeups) {
            cv->wakeups++;
            cv->cv.notify_one();
        }
        c.ret(0);
    });
    reg("pthread_cond_broadcast", [](Ctx& c) {
        GCond* cv = get_cond(c.arg(0));
        std::lock_guard lk(cv->m);
        cv->wakeups = cv->waiters;
        cv->cv.notify_all();
        c.ret(0);
    });
    reg("pthread_cond_wait", [](Ctx& c) { c.ret(u32(cond_wait(c.arg(0), c.arg(1), nullptr))); });
    reg("pthread_cond_timedwait", [](Ctx& c) {
        auto dl = abs_time(c.arg(2));
        c.ret(u32(cond_wait(c.arg(0), c.arg(1), &dl)));
    });

    // ---------------------------------------------------------- semaphores
    reg("sem_init", [](Ctx& c) {
        mem::w32(c.arg(0), c.arg(2));
        std::lock_guard lk(g_obj_lock);
        auto& p = g_sems[c.arg(0)];
        delete p;
        p = new GSem;
        p->count = c.arg(2);
        c.ret(0);
    });
    reg("sem_destroy", [](Ctx& c) {
        std::lock_guard lk(g_obj_lock);
        auto it = g_sems.find(c.arg(0));
        if (it != g_sems.end()) {
            delete it->second;
            g_sems.erase(it);
        }
        c.ret(0);
    });
    reg("sem_post", [](Ctx& c) {
        GSem* s = get_sem(c.arg(0));
        std::lock_guard lk(s->m);
        s->count++;
        mem::w32(c.arg(0), s->count);
        s->cv.notify_one();
        c.ret(0);
    });
    reg("sem_wait", [](Ctx& c) {
        GSem* s = get_sem(c.arg(0));
        std::unique_lock lk(s->m);
        s->cv.wait(lk, [&] { return s->count > 0; });
        s->count--;
        mem::w32(c.arg(0), s->count);
        c.ret(0);
    });
    reg("sem_trywait", [](Ctx& c) {
        GSem* s = get_sem(c.arg(0));
        std::lock_guard lk(s->m);
        if (!s->count) {
            set_errno(G_EAGAIN);
            return c.ret(0xFFFFFFFF);
        }
        s->count--;
        mem::w32(c.arg(0), s->count);
        c.ret(0);
    });
    reg("sem_timedwait", [](Ctx& c) {
        GSem* s = get_sem(c.arg(0));
        auto dl = abs_time(c.arg(1));
        std::unique_lock lk(s->m);
        if (!s->cv.wait_until(lk, dl, [&] { return s->count > 0; })) {
            set_errno(G_ETIMEDOUT);
            return c.ret(0xFFFFFFFF);
        }
        s->count--;
        mem::w32(c.arg(0), s->count);
        c.ret(0);
    });
    reg("sem_getvalue", [](Ctx& c) {
        GSem* s = get_sem(c.arg(0));
        std::lock_guard lk(s->m);
        mem::w32(c.arg(1), s->count);
        c.ret(0);
    });

    // ---------------------------------------------------------- thread attributes
    // pthread_attr_t: flags, stack_base, stack_size, guard_size, sched_policy, sched_priority
    reg("pthread_attr_init", [](Ctx& c) {
        u32 a = c.arg(0);
        std::memset(mem::host(a), 0, 24);
        mem::w32(a + 8, 1 << 20);
        mem::w32(a + 12, 4096);
        c.ret(0);
    });
    reg("pthread_attr_destroy", [](Ctx& c) { c.ret(0); });
    reg("pthread_attr_setdetachstate", [](Ctx& c) { mem::w32(c.arg(0), c.arg(1) ? 1 : 0); c.ret(0); });
    reg("pthread_attr_setstacksize", [](Ctx& c) { mem::w32(c.arg(0) + 8, c.arg(1)); c.ret(0); });
    reg("pthread_attr_setstack", [](Ctx& c) {
        mem::w32(c.arg(0) + 4, c.arg(1));
        mem::w32(c.arg(0) + 8, c.arg(2));
        c.ret(0);
    });
    reg("pthread_attr_getstack", [](Ctx& c) {
        mem::w32(c.arg(1), mem::r32(c.arg(0) + 4));
        mem::w32(c.arg(2), mem::r32(c.arg(0) + 8));
        c.ret(0);
    });
    reg("pthread_attr_setschedparam", [](Ctx& c) { mem::w32(c.arg(0) + 20, mem::r32(c.arg(1))); c.ret(0); });
    reg("pthread_getattr_np", [](Ctx& c) {
        u32 a = c.arg(1);
        std::memset(mem::host(a), 0, 24);
        auto& ti = cpu::thread();
        u32 base = ti.stack_base, size = ti.stack_size;
        {
            std::lock_guard lk(g_threads_lock);
            auto it = g_threads.find(c.arg(0));
            if (it != g_threads.end()) {
                base = it->second.stack_base;
                size = it->second.stack_size;
            }
        }
        mem::w32(a + 4, base);
        mem::w32(a + 8, size);
        c.ret(0);
    });
    reg("pthread_getschedparam", [](Ctx& c) {
        if (c.arg(1)) mem::w32(c.arg(1), 0);
        if (c.arg(2)) mem::w32(c.arg(2), 0);
        c.ret(0);
    });
    reg("pthread_setschedparam", [](Ctx& c) { c.ret(0); });

    // ---------------------------------------------------------- threads
    reg("pthread_create", [](Ctx& c) {
        u32 out = c.arg(0), attr = c.arg(1), start = c.arg(2), arg = c.arg(3);
        u32 stack_base = 0, stack_size = 1 << 20;
        if (attr) {
            stack_base = mem::r32(attr + 4);
            if (mem::r32(attr + 8)) stack_size = mem::r32(attr + 8);
        }
        bool own = false;
        if (!stack_base) {
            stack_size = std::max<u32>(stack_size, 256 << 10);
            stack_base = mem::map_alloc(stack_size);
            own = true;
        }
        u32 tid = g_next_thread_id++;
        {
            std::lock_guard lk(g_threads_lock);
            g_threads[tid] = {stack_base, stack_size};
        }
        if (out) mem::w32(out, tid);
        LOGI("pthread_create %u: start=%s stack=%08x+%x", tid, cpu::symbolize(start).c_str(), stack_base,
             stack_size);
        std::thread([=] {
            cpu::thread_attach(stack_base, stack_size);
            cpu::thread().tid = tid;
            mem::w32(cpu::thread().tls_area + 4, tid);
            cpu::call(start, {arg});
            LOGI("thread %u finished", tid);
            cpu::thread_detach();
            if (own) mem::map_free(stack_base, stack_size);
            std::lock_guard lk(g_threads_lock);
            g_threads.erase(tid);
        }).detach();
        c.ret(0);
    });
    reg("pthread_exit", [](Ctx& c) {
        LOGE("pthread_exit called from %s (not supported mid-call)", cpu::symbolize(c.lr()).c_str());
        std::abort();
    });
    reg("pthread_self", [](Ctx& c) { c.ret(cpu::thread().tid); });
    reg("pthread_key_create", [](Ctx& c) {
        std::lock_guard lk(g_key_lock);
        mem::w32(c.arg(0), g_next_key++);
        c.ret(0);
    });
    reg("pthread_key_delete", [](Ctx& c) { c.ret(0); });
    reg("pthread_getspecific", [](Ctx& c) {
        auto it = t_specific.find(c.arg(0));
        c.ret(it == t_specific.end() ? 0 : it->second);
    });
    reg("pthread_setspecific", [](Ctx& c) {
        t_specific[c.arg(0)] = c.arg(1);
        c.ret(0);
    });
    reg("pthread_once", [](Ctx& c) {
        u32 once = c.arg(0);
        std::lock_guard lk(g_once_lock);
        if (mem::r32(once) == 0) {
            mem::w32(once, 1);
            cpu::call(c.arg(1));
            mem::w32(once, 2);
        }
        c.ret(0);
    });

    // ---------------------------------------------------------- sleeping
    reg("nanosleep", [](Ctx& c) {
        u32 ts = c.arg(0);
        timing::sleep_ns(s64(s32(mem::r32(ts))) * 1000000000 + s32(mem::r32(ts + 4)));
        if (c.arg(1)) {
            mem::w32(c.arg(1), 0);
            mem::w32(c.arg(1) + 4, 0);
        }
        c.ret(0);
    });
    reg("usleep", [](Ctx& c) {
        timing::sleep_ns(s64(c.arg(0)) * 1000);
        c.ret(0);
    });
    reg("sched_yield", [](Ctx& c) {
        std::this_thread::yield();
        c.ret(0);
    });
}

}  // namespace hle
