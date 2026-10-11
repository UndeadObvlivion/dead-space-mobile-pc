import type { EngineInterface, Register } from 'claude-code'

// The released build: every release of the port replaces this one file, so the button always starts
// the newest version. (Forward slashes on purpose: a lost backslash escape once made this a dead path.)
const DIR = 'C:/Users/Dillon/Documents/deadspace-port/dist'
const EXE = 'deadspace.exe'

// Starts the game detached (through "start", so the call returns at once and the game outlives it).
async function play($: EngineInterface): Promise<boolean> {
  const dir = DIR.split('/').join(String.fromCharCode(92))
  const exe = dir + String.fromCharCode(92) + EXE

  try {
    const found = await $.fs.stat(exe).then(() => true, () => false)

    if (!found) {
      $.ui.toast(`Dead Space is not at ${exe}`)

      return false
    }

    const r = await $.process.run(['cmd', '/c', 'start', '', '/D', dir, exe], { cwd: dir, timeoutMs: 15000 })
    $.ui.toast(r.exitCode === 0 ? 'Dead Space started' : `Could not start Dead Space: ${r.stderr.trim() || 'exit ' + r.exitCode}`)

    return r.exitCode === 0
  } catch (err) {
    $.ui.toast(`Could not start Dead Space: ${String(err)}`)

    return false
  }
}

export const register: Register = on => {
  // The button sits in a row above the prompt box.
  on('session.start', async ($, e, next) => {
    const started = await next(e)
    await $.command.register({ name: 'play', description: 'Start the Dead Space port (dist/deadspace.exe)' })

    return started
  })

  on('command.run', { command: 'play' }, async $ => {
    const ok = await play($)

    return { text: ok ? 'Dead Space started.' : 'Dead Space did not start.' }
  })

  on('ui.render', { component: 'AbovePrompt' }, ($, e, next) => {
    if (e.props.hasSurvey) {
      return next(e)
    }

    const { Box, Button } = $.ui.resolve(e)

    return (
      <Box>
        <Button key="play" label="▶ Play Dead Space" onPress={() => play($)} />
      </Box>
    )
  })
}
