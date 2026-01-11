#include <devices/devices.h>
#include <status.h>
#include <string.h>

static device_t *device_list[MAX_DEVICES];
static size_t device_list_count = 0;   // faster to save hwo many devices we have than check every time we register a new one

/// @brief register a new device
/// @param device device structure
/// @return df
int device_register(device_t *device){
    if (!device || !device->name)
        return -DEV_EXISTS;
    if (device_list_count >= MAX_DEVICES)
        return -MAX_DEVICES_REACHED;

    for (size_t i = 0; i < device_list_count; i++){
        if (device_list[i]->name == device->name || 
            strcmp(device_list[i]->name, device->name) == 0){
            return -DEV_EXISTS;
        }
    }

    device_list[device_list_count++] = device;
    return 0;
}

/// @brief get a device by its name
/// @param name in name
/// @return return device structure pointer, null indicates an error.
device_t *device_get_by_name(const char *name){
    if (!name)
        return NULL;

    for (size_t i = 0; i < device_list_count; i++){
        if (strcmp(device_list[i]->name, name) == 0){
            return device_list[i];
        }
    }

    return NULL;
}