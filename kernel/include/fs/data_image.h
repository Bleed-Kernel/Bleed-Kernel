#pragma once

/// @brief look in the top of everything mounted for the extras image (data.img) and mount
/// it at /mnt/data if theres one and nothing is there yet. run again whenever a drive turns up
void data_image_scan(void);
