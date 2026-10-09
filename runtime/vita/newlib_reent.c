/* Link-time fix (-Wl,--wrap=_reclaim_reent) for VitaSDK's newlib threads. */
#include <reent.h>
#include <stdio.h>
#include <stdlib.h>

void __real__reclaim_reent(struct _reent *reent);

/* Each thread's reent has stdin, stdout and stderr FILEs of its own, on the
 * process's descriptors 0-2. An exited thread's reent is reclaimed when a new
 * thread takes its slot, and the reclaim closes every FILE the reent holds
 * (_cleanup_r walks them with _fclose_r): the process's descriptors 0-2 with
 * them. The first threads to come and go were the online ones (guest threads,
 * name lookups), and stderr went silent for good on the network screen
 * (2026-10-05). The thread's standard streams are flushed, their buffers
 * freed and left out of the walk instead. */
void __wrap__reclaim_reent(struct _reent *reent)
{
    if (reent && reent != _GLOBAL_REENT) {
        FILE *streams[] = {reent->_stdin, reent->_stdout, reent->_stderr};

        for (unsigned i = 0; i < sizeof(streams) / sizeof(streams[0]); ++i) {
            FILE *stream = streams[i];

            if (!stream || !stream->_flags)
                continue;

            _fflush_r(reent, stream);

            if (stream->_flags & __SMBF)
                _free_r(reent, stream->_bf._base);

            stream->_bf._base = NULL;
            stream->_flags = 0;
        }
    }

    __real__reclaim_reent(reent);
}
