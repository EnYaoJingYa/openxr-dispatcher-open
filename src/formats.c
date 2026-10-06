/* SPDX-License-Identifier: MIT
 * VkFormat -> OpenGL ES internal format, for GLES games that pass a Vulkan
 * format number to xrCreateSwapchain (OVRPort translates them too). The pairs
 * are the standard Vulkan/GL format equivalences (as in Khronos' KTX tools);
 * formats without a GLES equivalent (e.g. B8G8R8A8) are passed on unchanged
 * instead of aborting the game as OVRPort does. */
#include "dispatch.h"

static const struct {
    uint16_t vk;
    uint16_t gl;
} k_formats[] = {
    {2, 0x8056},   {4, 0x8d62},   {6, 0x8057},   {9, 0x8229},   {10, 0x8f94},  {13, 0x8232},  {14, 0x8231},
    {16, 0x822b},  {17, 0x8f95},  {20, 0x8238},  {21, 0x8237},  {23, 0x8051},  {24, 0x8f96},  {27, 0x8d7d},
    {28, 0x8d8f},  {29, 0x8c41},  {37, 0x8058},  {38, 0x8f97},  {41, 0x8d7c},  {42, 0x8d8e},  {43, 0x8c43},
    {64, 0x8059},  {68, 0x906f},  {74, 0x8234},  {75, 0x8233},  {76, 0x822d},  {81, 0x823a},  {82, 0x8239},
    {83, 0x822f},  {88, 0x8d77},  {89, 0x8d89},  {90, 0x881b},  {95, 0x8d76},  {96, 0x8d88},  {97, 0x881a},
    {98, 0x8236},  {99, 0x8235},  {100, 0x822e}, {101, 0x823c}, {102, 0x823b}, {103, 0x8230}, {104, 0x8d71},
    {105, 0x8d83}, {106, 0x8815}, {107, 0x8d70}, {108, 0x8d82}, {109, 0x8814}, {122, 0x8c3a}, {123, 0x8c3d},
    {124, 0x81a5}, {126, 0x8cac}, {127, 0x8d48}, {129, 0x88f0}, {130, 0x8cad}, {147, 0x9274}, {148, 0x9275},
    {149, 0x9276}, {150, 0x9277}, {151, 0x9278}, {152, 0x9279}, {153, 0x9270}, {154, 0x9271}, {155, 0x9272},
    {156, 0x9273},
};

bool xd_looks_like_vk_format(int64_t f)
{
    return (f >= 1 && f <= 184) || (f >= 1000000000 && f < 1100000000);
}

int64_t xd_vk_to_gl(int64_t vk)
{
    for (size_t i = 0; i < sizeof k_formats / sizeof k_formats[0]; i++)
        if (k_formats[i].vk == vk)
            return k_formats[i].gl;
    return 0;
}
