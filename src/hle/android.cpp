// liblog
#include "hle/hle.h"

namespace hle {

static void log_line(u32 prio, const char* tag, const std::string& msg) {
    static const char lv[] = "??VDIWEFS";
    std::string m = msg;
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    logging::write('G', "[%c/%s] %s", prio < 9 ? lv[prio] : '?', tag ? tag : "", m.c_str());
}

void register_android() {
    reg("__android_log_print", [](Ctx& c) {
        ArgReader a = ArgReader::slots(c, 3);
        log_line(c.arg(0), c.argstr(1), format(c.argstr(2), a));
        c.ret(1);
    });
    reg("__android_log_vprint", [](Ctx& c) {
        ArgReader a = ArgReader::valist(c.arg(3));
        log_line(c.arg(0), c.argstr(1), format(c.argstr(2), a));
        c.ret(1);
    });
    reg("__android_log_write", [](Ctx& c) {
        log_line(c.arg(0), c.argstr(1), c.argstr(2) ? c.argstr(2) : "");
        c.ret(1);
    });
}

}  // namespace hle
