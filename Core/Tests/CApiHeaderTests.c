#include <stddef.h>
#include <stdint.h>

#include "PhoneMECore.h"

_Static_assert(PHONEME_C_API_VERSION_MAJOR == 1u,
               "unexpected C API major version");
_Static_assert(PHONEME_C_API_VERSION == 0x00010300u,
               "C API version encoding changed");
_Static_assert(sizeof(PhoneMERuntimeRef) == sizeof(void*),
               "runtime reference must remain an opaque pointer");
_Static_assert(sizeof(((PhoneMELCDUIEvent*)0)->generation) == sizeof(uint64_t),
               "LCDUI generation must remain 64-bit");
_Static_assert(offsetof(PhoneMELCDUIEvent, text) <
                   offsetof(PhoneMELCDUIEvent, detail),
               "LCDUI event text/detail ABI order changed");
_Static_assert(sizeof(((PhoneMEPushLaunchRequest*)0)->request_id) ==
                   sizeof(uint64_t),
               "push request ID must remain 64-bit");
_Static_assert(sizeof(&phoneme_push_notify_connection_available_from_source) ==
                   sizeof(void*),
               "source-aware push notification must remain exported");
_Static_assert(sizeof(&phoneme_uninstall_suite) == sizeof(void*),
               "suite uninstall must remain exported");
_Static_assert(sizeof(&phoneme_install_jar_replacing) == sizeof(void*),
               "explicit suite replacement must remain exported");
_Static_assert(sizeof(&phoneme_copy_last_error_message) == sizeof(void*),
               "runtime error message accessor must remain exported");
_Static_assert(sizeof(&phoneme_copy_midlet_error_message) == sizeof(void*),
               "MIDlet error message accessor must remain exported");

int main(void) {
    PhoneMEPermissionResponse response = {
        PHONEME_PERMISSION_DENIED,
        PHONEME_PERMISSION_ONESHOT,
    };
    PhoneMEPushLaunchRequest request = {0};
    PhoneMELCDUIEvent event = {0};

    return (response.decision == PHONEME_PERMISSION_DENIED &&
            request.request_id == 0u && event.generation == 0u)
               ? 0
               : 1;
}
