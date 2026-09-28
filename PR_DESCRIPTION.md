# Implement an HLE stub for libScePlayerInvitationDialog

## Why

Games that load `libScePlayerInvitationDialog` currently have no HLE implementation for the invitation dialog. This can prevent startup or leave games waiting indefinitely for the dialog to finish.

## Changes

- Add the PRX module and its `Initialize`, `Open`, `UpdateStatus`, `GetStatus`, `Close`, and `Terminate` exports.
- Simulate the `None -> Initialized -> Running -> Finished` lifecycle: the first `UpdateStatus` after `Open` finishes the dialog without displaying UI or starting a network operation.
- Handle repeated or out-of-order calls and protect shared state with a mutex.
- Log calls and available arguments to the console without dereferencing the opening parameters, whose ABI layout has not been confirmed.
- Add a focused test covering opening, polling, closing, reopening, and out-of-order calls.

## Verification

```text
cmake --build build_ninja --target player_invitation_dialog_tests
ctest --test-dir build_ninja -R "^player_invitation_dialog$" --output-on-failure
ninja -C build_ninja core/libs/libs/libScePlayerInvitationDialog.prx
```

The focused test passes, and the NID-patched PRX builds successfully.

## Limitations

This stub does not display system UI or send invitations. `ScePlayerInvitationDialogParam` is opaque; its exact layout and any additional exports still need to be confirmed against traces or a compatible SDK.
