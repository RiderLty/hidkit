#ifndef HID_STRUC_H
#define HID_STRUC_H
#include <stdint.h>
#define MOUSE_LEFT 0x01
#define MOUSE_RIGHT 0x02
#define MOUSE_MIDDLE 0x04
#define MOUSE_FORWARD 0x08
#define MOUSE_BACKWARD 0x10


union mouse_buttons {
    uint8_t raw;
    struct {
        uint8_t left   : 1;
        uint8_t right  : 1;
        uint8_t middle : 1;
        uint8_t back   : 1;
        uint8_t forward  : 1;
        uint8_t reserved : 3;
    } __attribute__((packed));
};

typedef struct  {
    uint8_t report_id;
    union mouse_buttons buttons;
    int16_t x;
    int16_t y;
    int8_t wheel;
    uint8_t reserved;
} __attribute__((packed)) mouse_report_x8;


union keyboard_modifiers {
    uint8_t raw;
    struct {
        uint8_t left_ctrl   : 1;
        uint8_t left_shift  : 1;
        uint8_t left_alt    : 1;
        uint8_t left_gui    : 1;
        uint8_t right_ctrl  : 1;
        uint8_t right_shift : 1;
        uint8_t right_alt   : 1;
        uint8_t right_gui   : 1;
    } __attribute__((packed));
};
typedef struct  {
    union keyboard_modifiers modifiers; // Byte 0
    uint8_t reserved;                   // Byte 1，必须为0
    uint8_t keys[6];                    // Byte 2-7，最多6个键码
} __attribute__((packed)) keyboard_report_x8;


#endif