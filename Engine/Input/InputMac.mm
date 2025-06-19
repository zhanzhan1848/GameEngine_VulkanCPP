#include "Input.h"
#include "InputMac.h"

namespace primal::input
{
    namespace
    {
        constexpr u32 vk_mapping[256]
        {
            /* 0x00 */  input_code::key_a,                           //VKEY_A,
            /* 0x01 */  input_code::key_s,                           //VKEY_S,
            /* 0x02 */  input_code::key_d,                           //VKEY_D,
            /* 0x03 */  input_code::key_f,                           //VKEY_F,
            /* 0x04 */  input_code::key_h,                           //VKEY_H,
            /* 0x05 */  input_code::key_g,                           //VKEY_G,
            /* 0x06 */  input_code::key_z,                           //VKEY_Z,
            /* 0x07 */  input_code::key_x,                           //VKEY_X,
            /* 0x08 */  input_code::key_c,                           //VKEY_C,
            /* 0x09 */  input_code::key_v,                           //VKEY_V,
            /* 0x0A */  u32_invalid_id,                           //VKEY_OEM_3,  // Section key.
            /* 0x0B */  input_code::key_b,                           //VKEY_B,
            /* 0x0C */  input_code::key_q,                           //VKEY_Q,
            /* 0x0D */  input_code::key_w,                           //VKEY_W,
            /* 0x0E */  input_code::key_e,                           //VKEY_E,
            /* 0x0F */  input_code::key_r,                           //VKEY_R,
            /* 0x10 */  input_code::key_y,                           //VKEY_Y,
            /* 0x11 */  input_code::key_t,                           //VKEY_T,
            /* 0x12 */  input_code::key_1,                           //VKEY_1,
            /* 0x13 */  input_code::key_2,                           //VKEY_2,
            /* 0x14 */  input_code::key_3,                           //VKEY_3,
            /* 0x15 */  input_code::key_4,                           //VKEY_4,
            /* 0x16 */  input_code::key_6,                           //VKEY_6,
            /* 0x17 */  input_code::key_5,                           //VKEY_5,
            /* 0x18 */  input_code::key_plus,                           //VKEY_OEM_PLUS,  // =+
            /* 0x19 */  input_code::key_9,                           //VKEY_9,
            /* 0x1A */  input_code::key_7,                           //VKEY_7,
            /* 0x1B */  input_code::key_minus,                           //VKEY_OEM_MINUS,  // -_
            /* 0x1C */  input_code::key_8,                           //VKEY_8,
            /* 0x1D */  input_code::key_0,                           //VKEY_0,
            /* 0x1E */  u32_invalid_id,                           //VKEY_OEM_6,  // ]}
            /* 0x1F */  input_code::key_o,                           //VKEY_O,
            /* 0x20 */  input_code::key_u,                           //VKEY_U,
            /* 0x21 */  u32_invalid_id,                           //VKEY_OEM_4,  // {[
            /* 0x22 */  input_code::key_i,                           //VKEY_I,
            /* 0x23 */  input_code::key_p,                           //VKEY_P,
            /* 0x24 */  input_code::key_return,                           //VKEY_RETURN,  // Return
            /* 0x25 */  input_code::key_l,                           //VKEY_L,
            /* 0x26 */  input_code::key_j,                           //VKEY_J,
            /* 0x27 */  u32_invalid_id,                           //VKEY_OEM_7,  // '"
            /* 0x28 */  input_code::key_k,                           //VKEY_K,
            /* 0x29 */  u32_invalid_id,                           //VKEY_OEM_1,      // ;:
            /* 0x2A */  u32_invalid_id,                           //VKEY_OEM_5,      // \|
            /* 0x2B */  u32_invalid_id,                           //VKEY_OEM_COMMA,  // ,<
            /* 0x2C */  u32_invalid_id,                           //VKEY_OEM_2,      // /?
            /* 0x2D */  input_code::key_n,                           //VKEY_N,
            /* 0x2E */  input_code::key_m,                           //VKEY_M,
            /* 0x2F */  u32_invalid_id,                           //VKEY_OEM_PERIOD,  // .>
            /* 0x30 */  input_code::key_tab,                           //VKEY_TAB,
            /* 0x31 */  input_code::key_backspace,                           //VKEY_SPACE,
            /* 0x32 */  u32_invalid_id,                           //VKEY_OEM_3,    // `~
            /* 0x33 */  u32_invalid_id,                           //VKEY_BACK,     // Backspace
            /* 0x34 */  u32_invalid_id,                           //VKEY_UNKNOWN,  // n/a
            /* 0x35 */  input_code::key_escape,                           //VKEY_ESCAPE,
            /* 0x36 */  input_code::key_atl,                           //VKEY_APPS,     // Right Command
            /* 0x37 */  input_code::key_atl,                           //VKEY_LWIN,     // Left Command
            /* 0x38 */  input_code::key_shift,                           //VKEY_SHIFT,    // Left Shift
            /* 0x39 */  input_code::key_capslock,                           //VKEY_CAPITAL,  // Caps Lock
            /* 0x3A */  u32_invalid_id,                           //VKEY_MENU,     // Left Option
            /* 0x3B */  input_code::key_control,                           //VKEY_CONTROL,  // Left Ctrl
            /* 0x3C */  input_code::key_shift,                           //VKEY_SHIFT,    // Right Shift
            /* 0x3D */  u32_invalid_id,                           //VKEY_MENU,     // Right Option
            /* 0x3E */  input_code::key_control,                           //VKEY_CONTROL,  // Right Ctrl
            /* 0x3F */  u32_invalid_id,                           //VKEY_UNKNOWN,  // fn
            /* 0x40 */  u32_invalid_id,                           //VKEY_F17,
            /* 0x41 */  u32_invalid_id,                           //VKEY_DECIMAL,   // Num Pad .
            /* 0x42 */  u32_invalid_id,                           //VKEY_UNKNOWN,   // n/a
            /* 0x43 */  u32_invalid_id,                           //VKEY_MULTIPLY,  // Num Pad *
            /* 0x44 */  u32_invalid_id,                           //VKEY_UNKNOWN,   // n/a
            /* 0x45 */  u32_invalid_id,                           //VKEY_ADD,       // Num Pad +
            /* 0x46 */  u32_invalid_id,                           //VKEY_UNKNOWN,   // n/a
            /* 0x47 */  u32_invalid_id,                           //VKEY_CLEAR,     // Num Pad Clear
            /* 0x48 */  u32_invalid_id,                           //VKEY_VOLUME_UP,
            /* 0x49 */  u32_invalid_id,                           //VKEY_VOLUME_DOWN,
            /* 0x4A */  u32_invalid_id,                           //VKEY_VOLUME_MUTE,
            /* 0x4B */  u32_invalid_id,                           //VKEY_DIVIDE,    // Num Pad /
            /* 0x4C */  input_code::key_return,                           //VKEY_RETURN,    // Num Pad Enter
            /* 0x4D */  u32_invalid_id,                           //VKEY_UNKNOWN,   // n/a
            /* 0x4E */  u32_invalid_id,                           //VKEY_SUBTRACT,  // Num Pad -
            /* 0x4F */  u32_invalid_id,                           //VKEY_F18,
            /* 0x50 */  u32_invalid_id,                           //VKEY_F19,
            /* 0x51 */  u32_invalid_id,                           //VKEY_OEM_PLUS,  // Num Pad =.
            /* 0x52 */  u32_invalid_id,                           //VKEY_NUMPAD0,
            /* 0x53 */  u32_invalid_id,                           //VKEY_NUMPAD1,
            /* 0x54 */  u32_invalid_id,                           //VKEY_NUMPAD2,
            /* 0x55 */  u32_invalid_id,                           //VKEY_NUMPAD3,
            /* 0x56 */  u32_invalid_id,                           //VKEY_NUMPAD4,
            /* 0x57 */  u32_invalid_id,                           //VKEY_NUMPAD5,
            /* 0x58 */  u32_invalid_id,                           //VKEY_NUMPAD6,
            /* 0x59 */  u32_invalid_id,                           //VKEY_NUMPAD7,
            /* 0x5A */  u32_invalid_id,                           //VKEY_F20,
            /* 0x5B */  u32_invalid_id,                           //VKEY_NUMPAD8,
            /* 0x5C */  u32_invalid_id,                           //VKEY_NUMPAD9,
            /* 0x5D */  u32_invalid_id,                           //VKEY_UNKNOWN,  // Yen (JIS Keyboard Only)
            /* 0x5E */  u32_invalid_id,                           //VKEY_UNKNOWN,  // Underscore (JIS Keyboard Only)
            /* 0x5F */  u32_invalid_id,                           //VKEY_UNKNOWN,  // KeypadComma (JIS Keyboard Only)
            /* 0x60 */  input_code::key_f5,                           //VKEY_F5,
            /* 0x61 */  input_code::key_f6,                           //VKEY_F6,
            /* 0x62 */  input_code::key_f7,                           //VKEY_F7,
            /* 0x63 */  input_code::key_f3,                           //VKEY_F3,
            /* 0x64 */  input_code::key_f8,                           //VKEY_F8,
            /* 0x65 */  input_code::key_f9,                           //VKEY_F9,
            /* 0x66 */  u32_invalid_id,                           //VKEY_UNKNOWN,  // Eisu (JIS Keyboard Only)
            /* 0x67 */  input_code::key_f11,                           //VKEY_F11,
            /* 0x68 */  u32_invalid_id,                           //VKEY_UNKNOWN,  // Kana (JIS Keyboard Only)
            /* 0x69 */  u32_invalid_id,                           //VKEY_F13,
            /* 0x6A */  u32_invalid_id,                           //VKEY_F16,
            /* 0x6B */  u32_invalid_id,                           //VKEY_F14,
            /* 0x6C */  u32_invalid_id,                           //VKEY_UNKNOWN,  // n/a
            /* 0x6D */  input_code::key_f10,                           //VKEY_F10,
            /* 0x6E */  u32_invalid_id,                           //VKEY_APPS,  // Context Menu key
            /* 0x6F */  input_code::key_f12,                           //VKEY_F12,
            /* 0x70 */  u32_invalid_id,                           //VKEY_UNKNOWN,  // n/a
            /* 0x71 */  u32_invalid_id,                           //VKEY_F15,
            /* 0x72 */  input_code::key_insert,                           //VKEY_INSERT,  // Help
            /* 0x73 */  input_code::key_home,                           //VKEY_HOME,    // Home
            /* 0x74 */  input_code::key_page_up,                           //VKEY_PRIOR,   // Page Up
            /* 0x75 */  input_code::key_delete,                           //VKEY_DELETE,  // Forward Delete
            /* 0x76 */  input_code::key_f4,                           //VKEY_F4,
            /* 0x77 */  input_code::key_end,                           //VKEY_END,  // End
            /* 0x78 */  input_code::key_f2,                           //VKEY_F2,
            /* 0x79 */  u32_invalid_id,                           //VKEY_NEXT,  // Page Down
            /* 0x7A */  input_code::key_f1,                           //VKEY_F1,
            /* 0x7B */  input_code::key_left,                           //VKEY_LEFT,    // Left Arrow
            /* 0x7C */  input_code::key_right,                           //VKEY_RIGHT,   // Right Arrow
            /* 0x7D */  input_code::key_down,                           //VKEY_DOWN,    // Down Arrow
            /* 0x7E */  input_code::key_up,                           //VKEY_UP,      // Up Arrow
            /* 0x7F */  u32_invalid_id                           //VKEY_UNKNOWN  // n/a
        };

        struct modifier_flags
        {
            enum flags : u8
            {
                left_shift = 0x10,
                left_control = 0x20,
                left_atl = 0x40,
                right_shift = 0x01,
                right_control = 0x02,
                right_atl = 0x04,
            };
        };

        u8 modifier_keys_state{ 0 };

        void set_modifier_input(NSEvent* event, input_code::code code, modifier_flags::flags flag) {
            if (event.modifierFlags & flag) 
            {
                set(input_source::keyboard, code, { 1.f, 0.f, 0.f });
                modifier_keys_state |= flag;
            } 
            else if (modifier_keys_state & flag) 
            {
                set(input_source::keyboard, code, { 0.f, 0.f, 0.f });
                modifier_keys_state &= ~flag;
            }
        }

        void set_modifier_inputs(NSEvent* event, input_code::code code) 
        {
            if (code == input_code::key_shift) 
            {
                set_modifier_input(event, input_code::key_left_shift, modifier_flags::left_shift);
                set_modifier_input(event, input_code::key_right_shift, modifier_flags::right_shift);
            } 
            else if (code == input_code::key_control) 
            {
                set_modifier_input(event, input_code::key_left_control, modifier_flags::left_control);
                set_modifier_input(event, input_code::key_right_control, modifier_flags::right_control);
            } 
            else if (code == input_code::key_atl) 
            {
                set_modifier_input(event, input_code::key_left_atl, modifier_flags::left_atl);
                set_modifier_input(event, input_code::key_right_atl, modifier_flags::right_atl);
            }
        }

        math::v2 get_mouse_position(NSEvent* event) 
        {
            NSPoint pos = [event locationInWindow];
            return { (f32)pos.x, (f32)pos.y };
        }
    } // anonymous namespace

    void process_input_message(NSEvent* event) 
    {
        switch ([event type]) 
        {
            case NSEventTypeKeyDown: 
            {
                unsigned short keyCode = [event keyCode];
                if (keyCode < 256) 
                {
                    input_code::code code = static_cast<input_code::code>(vk_mapping[keyCode]);
                    if (code != u32_invalid_id) 
                    {
                        set(input_source::keyboard, code, {1.0f, 0.0f, 0.0f});
                        set_modifier_inputs(event, code);
                    }
                }
                break;
            }
            case NSEventTypeKeyUp: 
            {
                unsigned short keyCode = [event keyCode];
                if (keyCode < 256) 
                {
                    input_code::code code = static_cast<input_code::code>(vk_mapping[keyCode]);
                    if (code != u32_invalid_id) 
                    {
                        set(input_source::keyboard, code, {0.0f, 0.0f, 0.0f});
                        set_modifier_inputs(event, code);
                    }
                }
                break;
            }
            case NSEventTypeMouseMoved:
            case NSEventTypeLeftMouseDragged:
            case NSEventTypeRightMouseDragged:
            case NSEventTypeOtherMouseDragged: 
            {
                math::v2 pos = get_mouse_position(event);
                set(input_source::mouse, input_code::mouse_position_x, { pos.x(), 0.f, 0.f });
                set(input_source::mouse, input_code::mouse_position_y, { pos.y(), 0.f, 0.f });
                set(input_source::mouse, input_code::mouse_position, { pos.x(), pos.y(), 0.f });
                break;
            }
            case NSEventTypeLeftMouseDown: 
            {
                math::v2 pos = get_mouse_position(event);
                set(input_source::mouse, input_code::mouse_left, { pos.x(), pos.y(), 1.f });
                break;
            }
            case NSEventTypeLeftMouseUp: 
            {
                math::v2 pos = get_mouse_position(event);
                set(input_source::mouse, input_code::mouse_left, { pos.x(), pos.y(), 0.f });
                break;
            }
            case NSEventTypeRightMouseDown: 
            {
                math::v2 pos = get_mouse_position(event);
                set(input_source::mouse, input_code::mouse_rigth, { pos.x(), pos.y(), 1.f });
                break;
            }
            case NSEventTypeRightMouseUp: 
            {
                math::v2 pos = get_mouse_position(event);
                set(input_source::mouse, input_code::mouse_rigth, { pos.x(), pos.y(), 0.f });
                break;
            }
            case NSEventTypeOtherMouseDown: 
            { // Middle mouse button
                math::v2 pos = get_mouse_position(event);
                set(input_source::mouse, input_code::mouse_middle, { pos.x(), pos.y(), 1.f });
                break;
            }
            case NSEventTypeOtherMouseUp: 
            {
                math::v2 pos = get_mouse_position(event);
                set(input_source::mouse, input_code::mouse_middle, { pos.x(), pos.y(), 0.f });
                break;
            }
            case NSEventTypeScrollWheel: 
            {
                float deltaY = [event scrollingDeltaY];
                set(input_source::mouse, input_code::mouse_wheel, { deltaY, 0.f, 0.f });
                break;
            }
            default:
                break;
        }
    }
}