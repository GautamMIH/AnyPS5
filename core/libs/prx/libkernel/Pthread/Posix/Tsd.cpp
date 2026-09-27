#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "../include/PthreadSync.hpp"

extern "C" {

void* APS5_VABI pthread_getspecific_nid_postfix(PthreadKey key) {
    return PthreadSync::KeyGet(key);
}

int APS5_VABI pthread_setspecific_nid_postfix(PthreadKey key, void* value) {
    return PthreadSync::KeySet(key, value);
}

int APS5_VABI pthread_key_create_nid_postfix(PthreadKey* key, pthread_key_destructor_func_t destructor) {
    return PthreadSync::KeyCreate(key, destructor);
}

int APS5_VABI pthread_key_delete_nid_postfix(PthreadKey key) {
    return PthreadSync::KeyDelete(key);
}

}
