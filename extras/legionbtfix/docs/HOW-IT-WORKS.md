# How LegionBtFix works

Addresses below are in the x86_64 slice of `/usr/sbin/bluetoothd` from macOS 26.7.1 (25G241), file sha256 `4f27c3b3fab84df24d97b9fb7dd47e7ca07d173ec2a6afb1e36cf1a646bcb18b`, fat offset 0x4000. Everything was found with static disassembly (`llvm-objdump` 20) and `bluetoothd`'s own packet logger; nothing was attached to the running process.

## Patch 1: LE own address

### The evidence

An HCI trace of the crash loop shows `bluetoothd` sending LE Create Connection (0x200D) with Own_Address_Type 1 (random) 55 s before it ever sends LE Set Random Address (0x2005). The MT7925 answers each one with Command Status 0x12, Invalid HCI Command Parameters, which is what the Core specification requires when no random address has been set. `bluetoothd` maps that to its internal "STATUS 718", treats it as fatal inside its LE Connection Complete handler, and aborts. launchd restarts it and the sequence repeats. The same command with Own_Address_Type 0 (public) is accepted every time.

### The patch

In the LE connection manager (function at 0x1006cf0fe), the choice between public and random comes from the helper at 0x1006e9fd8 (the one that logs "Using public address because device ... isn't a cloud pair device" and "Still using random address to connect"). Its boolean result (1 = public) is passed to the Stack.LE auto-connect at 0x10021a5d2:

```
1006cfca5  call   0x1006e9fd8
1006cfcaa  mov    ecx, 0xff ; mov [rsp+8], ecx ; mov [rsp], ecx
1006cfcb6  movzx  ecx, al          0f b6 c8   ->   mov cl, 1 ; nop   b1 01 90
```

`ecx` holds 0xff before, so after `mov cl, 1` it is exactly 1: always public. Nothing else in the function changes. The page is at slice offset 0x6cf000 (fat 0x6d3000); the patch sits at in-page 0xca2 within a 43-byte window; page SHA-1 `27ef9cab...707b`.

## Patch 2: BT clock

### The evidence

Before every AVDTP START, the A2DP code (0x1004489a2) calls `OI_HCI_ReadClock(cb, &device->bdaddr, which = 0)`: Which_Clock 0 is the local clock. The stack turns the BD address into the ACL handle (0x0033 on air). The MT7925 answers with status 0, a valid clock and accuracy, but Connection_Handle 0x0000.

### The path from the reply to the error

1. The Command Complete parser `readClockCB` (0x10017d2ad, named from its assert string) reads status, handle, clock and accuracy, then looks the link up **by the returned handle** (0x10017fb40 walks the ACL link lists comparing the 16-bit handle at offset 0). Handle 0 matches nothing, so `link` is not a live link.
2. The callback (0x100448f30) takes `mach_absolute_time()` and queues a block (invoke 0x100457c09) with link, host time, status, clock and accuracy.
3. The block calls 0x10015f8b0(link, &bdaddr), which checks that the link is in the live list and returns 0x515 when it is not. On non-zero it logs `Failed to read BT clock` and **skips** 0x10044fe4e.
4. 0x10044fe4e is the store. Its arguments are status, clock, accuracy and host time only; the link is not passed. With status 0 it takes the A2DP object's lock, scales the clock and works with the host time minus the time the command was sent. With a non-zero status it logs that status and stores nothing.

So the link check is only a gate, and the values it guards are correct.

### The patch

```
100457c4d  45 85 f6     test  r14d, r14d
100457c50  74 25        je    0x100457c77      ->   eb 25   jmp 0x100457c77
```

The block now always takes the success path. The Mac is central on the AirPods link (Role Discovery 0x00 in the trace), so the local clock is the piconet clock the stream is timed against. A non-zero HCI status still ends in 0x10044fe4e's own error branch. All three callers of `OI_HCI_ReadClock` with this callback go through the same block, so one byte covers them. Page at slice offset 0x457000 (fat 0x45b000), patch at in-page 0xc29 within a 51-byte window that is unique in the fat file, page SHA-1 `8ac81e18d6b1e039dd296826b8eb4987b8302481`.

Other fixes considered: rewriting the handle in the reply at the transport (not possible; MtkBt lets go of the USB interface after the firmware download and `bluetoothd` reads HCI events from user space); patching `readClockCB` to keep the handle it asked for (the request carries a BD address, not a handle, so there is nothing to compare without more code); the firmware (MediaTek's, no change possible).

## Gating

Each patch is applied only to a page that passes all of:

- the vnode path is `/usr/sbin/bluetoothd` and the kernel is Tahoe 25.x (Lilu min and max kernel);
- the bytes at the patch's in-page offset match its window exactly where the patched bytes are not; call and stack displacements, which change with every build, are part of the exact match. The patched bytes may be either the original or the patched form, so an already patched page is recognised and left alone;
- the SHA-1 of the whole 4 KB page equals the one taken from 25G241. Neither page holds a BlueToolFixup pattern (vendor check, chipset checks, NVRAM check, address check), so the hash is of untouched bytes whatever order the two plugins run in.

Any other build fails one of those and the plugin does nothing. If only the window matches, it logs `page hash differs` so you know a re-take is needed.

## Re-taking the windows for another build

1. `sha256sum /usr/sbin/bluetoothd`, then find the x86_64 slice offset with `otool -f`.
2. Find the LE own address site: search the disassembly for the string "Still using random address to connect", find the function that calls its helper, and look for the `movzx ecx, al` right after the call, preceded by the `mov ecx, 0xff` stores.
3. Find the BT clock site: search for "Failed to read BT clock", find the block that logs it, and look for the `test r14d, r14d ; je` pair (register may differ) right before the call to the store function.
4. For each site, copy a window of 40 to 60 bytes that is unique in the whole fat file (check with a hex search), note the in-page offset, and take the SHA-1 of the 4 KB page containing it.
5. Put the new values in `src/LegionBtFix.cpp`, bump the kernel gate if the major version changed, rebuild, and check the `lbtfx` log lines after boot.

## How the patch is applied

`src/LegionBtFix.cpp` registers with Lilu, routes `_cs_validate_page` (the function the kernel calls to validate each page of a code-signed file as it is paged in) and, for pages of the target file, compares and rewrites the bytes in the page buffer before the kernel's validation runs. That is the BlueToolFixup mechanism, which is why this plugin is GPL-2.0.

## The remaining link drop

With both patches in, the A2DP stream starts correctly every time. Whether sound is heard depends on something else: in three of three runs with `bluetoothd`'s HCI packet logging with raw audio enabled the AirPods played, and in three of three runs without it the controller stopped completing ACL packets right after the first media packet and dropped the link with reason 0x08 after about 10 s. The RTP timestamps are anchored in both cases, so the clock patch is doing its job. The logging flag is read only inside `bluetoothd`'s packet-record builder (how many payload bytes to copy), so the difference is timing or chance. Working leads: Wi-Fi and Bluetooth coexistence on the shared MT7925 radio (the Wi-Fi driver sends no coexistence information to the Bluetooth firmware), and USB bulk timing. `tools/bt-hci-logging.sh` applies the workaround; `tools/pklg-*.py` read the traces it produces.
