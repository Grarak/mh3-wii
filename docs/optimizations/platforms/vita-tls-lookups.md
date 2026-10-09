# Thread-local storage on the Vita

## What the original does

The Wii's OS has no thread-local storage in the C library's sense; the game
keeps per-thread state in its own thread records. TLS only appears with the
port's C and C++ runtime.

## What the port does instead

VitaSDK's prebuilt libraries (newlib's reent, pthread-embedded, libstdc++)
use emulated TLS, and every `errno`, `__getreent` or `pthread_getspecific`
goes through a long chain:

```text
__emutls_get_address
  -> __gthread_getspecific -> pthread_getspecific -> pte_osTlsGetValue
  -> vitasdk_get_tls_data -> __getreent_for_thread -> sceKernelGetTLSAddr(0x89)
```

`__getreent_for_thread` saves eight core registers and `d8` and reserves 136
bytes of stack even when its slot is already initialized. The port:

- **Has no `thread_local`** in its own code. Per-thread guest state is found
  by comparing the stack pointer with each thread's stack range
  ([guest worker](../threading/guest-worker.md)); locks are non-recursive
  spin locks, and nothing calls `pthread_self` on a hot path.
- **Shortens the SDK's initialized lookups** with two link wraps in
  `runtime/vita/fast_tls.c` (`MH3_VITA_FAST_TLS`, on by default):
  `__wrap___getreent` reads slot `0x89` and returns the reent pointer, and
  `__wrap_pte_osTlsGetValue` reads the same slot and scans the SDK's key
  list directly. Either tail-calls the original when the slot is not
  initialized. They only read SDK data: no cache, no extra key, nothing that
  outlives a thread's cleanup.

## Why

On the initialized path a key lookup whose first node matches runs 15
instructions with one nested call and 16 bytes of stack, against 30, 3 and
192 in the SDK. Removing `thread_local` shrank per-thread TLS from 73 KiB to
52 bytes. In an Alatreon profile afterwards, TLS helpers were 0.2% of the
game thread's samples and 1.2% of the render thread's.

## Example

The layouts the wrappers rely on (VitaSDK's newlib `threading.c` and
pthread-embedded's `vita_osal.c`): slot `0x89` holds the current
`struct _reent *`; `tls_data_ext` and `pthread_data_ext` sit right before
it, so `vitasdk_get_tls_data` is `reent - 8`; each key node is
`{next, key, value}` at offsets 0, 4 and 8. Compile-time assertions check
the pointer size and node offsets. GNU `--wrap` changes undefined
references only, so the link flags are:

```text
-Wl,--wrap=__getreent
-Wl,--wrap=pte_osTlsGetValue
```

## Applying it to another Wii game

Generic: any Vita program built with VitaSDK. Avoid `thread_local` and
recursive mutexes in hot code, and wrap the SDK's lookups if errno or
reent access shows up in profiles. The wraps depend on VitaSDK's private
layouts: review them when upgrading VitaSDK.
