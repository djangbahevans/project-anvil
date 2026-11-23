#include "anvil/ui/key_translation.hpp"

#include <array>
#include <span>

namespace anvil::ui {
namespace {

using core::Key;

struct QtKey {
    int qt;
    Key key;
};

// The keys that are the same on every keyboard layout, apart from the numeric keypad.
constexpr QtKey fixed_keys[] = {
    {Qt::Key_Backspace, Key::backspace},
    {Qt::Key_Tab, Key::tab},
    {Qt::Key_Backtab, Key::tab},
    {Qt::Key_Return, Key::enter},
    {Qt::Key_Enter, Key::enter},
    {Qt::Key_Pause, Key::pause_break},
    {Qt::Key_CapsLock, Key::caps_lock},
    {Qt::Key_Escape, Key::escape},
    {Qt::Key_Space, Key::space},
    {Qt::Key_PageUp, Key::page_up},
    {Qt::Key_PageDown, Key::page_down},
    {Qt::Key_End, Key::end},
    {Qt::Key_Home, Key::home},
    {Qt::Key_Left, Key::left_arrow},
    {Qt::Key_Up, Key::up_arrow},
    {Qt::Key_Right, Key::right_arrow},
    {Qt::Key_Down, Key::down_arrow},
    {Qt::Key_Insert, Key::insert},
    {Qt::Key_Delete, Key::delete_key},
    {Qt::Key_Menu, Key::context_menu},
    {Qt::Key_NumLock, Key::num_lock},
    {Qt::Key_ScrollLock, Key::scroll_lock},
    {Qt::Key_Control, Key::ctrl},
    {Qt::Key_Shift, Key::shift},
    {Qt::Key_Alt, Key::alt},
    {Qt::Key_Meta, Key::meta},
    {Qt::Key_Clear, Key::clear},
};

// The keys of the punctuation, as a US keyboard makes them without Shift and with it. A key that
// Qt names by the character it makes goes back to the key.
constexpr QtKey punctuation_keys[] = {
    {Qt::Key_Semicolon, Key::semicolon},
    {Qt::Key_Colon, Key::semicolon},
    {Qt::Key_Equal, Key::equal},
    {Qt::Key_Plus, Key::equal},
    {Qt::Key_Comma, Key::comma},
    {Qt::Key_Less, Key::comma},
    {Qt::Key_Minus, Key::minus},
    {Qt::Key_Underscore, Key::minus},
    {Qt::Key_Period, Key::period},
    {Qt::Key_Greater, Key::period},
    {Qt::Key_Slash, Key::slash},
    {Qt::Key_Question, Key::slash},
    {Qt::Key_QuoteLeft, Key::backquote},
    {Qt::Key_AsciiTilde, Key::backquote},
    {Qt::Key_BracketLeft, Key::bracket_left},
    {Qt::Key_BraceLeft, Key::bracket_left},
    {Qt::Key_Backslash, Key::backslash},
    {Qt::Key_Bar, Key::backslash},
    {Qt::Key_BracketRight, Key::bracket_right},
    {Qt::Key_BraceRight, Key::bracket_right},
    {Qt::Key_Apostrophe, Key::quote},
    {Qt::Key_QuoteDbl, Key::quote},
};

// The digits, and what the same keys make with Shift held.
constexpr QtKey digit_keys[] = {
    {Qt::Key_0, Key::digit0}, {Qt::Key_ParenRight, Key::digit0},
    {Qt::Key_1, Key::digit1}, {Qt::Key_Exclam, Key::digit1},
    {Qt::Key_2, Key::digit2}, {Qt::Key_At, Key::digit2},
    {Qt::Key_3, Key::digit3}, {Qt::Key_NumberSign, Key::digit3},
    {Qt::Key_4, Key::digit4}, {Qt::Key_Dollar, Key::digit4},
    {Qt::Key_5, Key::digit5}, {Qt::Key_Percent, Key::digit5},
    {Qt::Key_6, Key::digit6}, {Qt::Key_AsciiCircum, Key::digit6},
    {Qt::Key_7, Key::digit7}, {Qt::Key_Ampersand, Key::digit7},
    {Qt::Key_8, Key::digit8}, {Qt::Key_Asterisk, Key::digit8},
    {Qt::Key_9, Key::digit9}, {Qt::Key_ParenLeft, Key::digit9},
};

std::optional<Key> keypad_key(int qt_key) {
    if (qt_key >= Qt::Key_0 && qt_key <= Qt::Key_9) {
        return static_cast<Key>(static_cast<int>(Key::numpad0) + (qt_key - Qt::Key_0));
    }
    switch (qt_key) {
    case Qt::Key_Asterisk:
        return Key::numpad_multiply;
    case Qt::Key_Plus:
        return Key::numpad_add;
    case Qt::Key_Minus:
        return Key::numpad_subtract;
    case Qt::Key_Period:
    case Qt::Key_Comma:
        return Key::numpad_decimal;
    case Qt::Key_Slash:
        return Key::numpad_divide;
    case Qt::Key_Enter:
    case Qt::Key_Return:
        return Key::enter;
    default:
        return std::nullopt;
    }
}

#ifdef Q_OS_LINUX
// The place of a key on a PC keyboard, by the scan code that X11 and Wayland hand to Qt: the code
// of the Linux input system plus eight, which names the key of a US keyboard that is at that place.
std::optional<Key> key_at_scan_code(quint32 scan_code) {
    constexpr Key row_digits[] = {Key::digit1, Key::digit2, Key::digit3, Key::digit4,
                                  Key::digit5, Key::digit6, Key::digit7, Key::digit8,
                                  Key::digit9, Key::digit0, Key::minus,  Key::equal};
    constexpr Key row_top[] = {Key::key_q, Key::key_w, Key::key_e,        Key::key_r,
                               Key::key_t, Key::key_y, Key::key_u,        Key::key_i,
                               Key::key_o, Key::key_p, Key::bracket_left, Key::bracket_right};
    constexpr Key row_home[] = {Key::key_a, Key::key_s,     Key::key_d, Key::key_f,
                                Key::key_g, Key::key_h,     Key::key_j, Key::key_k,
                                Key::key_l, Key::semicolon, Key::quote, Key::backquote};
    constexpr Key row_bottom[] = {Key::key_z, Key::key_x, Key::key_c, Key::key_v,  Key::key_b,
                                  Key::key_n, Key::key_m, Key::comma, Key::period, Key::slash};
    if (scan_code < 8) {
        return std::nullopt;
    }
    const quint32 code = scan_code - 8;
    if (code >= 2 && code <= 13) {
        return row_digits[code - 2];
    }
    if (code >= 16 && code <= 27) {
        return row_top[code - 16];
    }
    if (code >= 30 && code <= 41) {
        return row_home[code - 30];
    }
    if (code == 43) {
        return Key::backslash;
    }
    if (code >= 44 && code <= 53) {
        return row_bottom[code - 44];
    }
    return std::nullopt;
}
#endif

} // namespace

std::optional<Key> key_from_qt(int qt_key, Qt::KeyboardModifiers modifiers) {
    if (modifiers & Qt::KeypadModifier) {
        if (const auto key = keypad_key(qt_key)) {
            return key;
        }
    }
    if (qt_key >= Qt::Key_A && qt_key <= Qt::Key_Z) {
        return static_cast<Key>(static_cast<int>(Key::key_a) + (qt_key - Qt::Key_A));
    }
    if (qt_key >= Qt::Key_F1 && qt_key <= Qt::Key_F24) {
        return static_cast<Key>(static_cast<int>(Key::f1) + (qt_key - Qt::Key_F1));
    }
    const auto find = [&](std::span<const QtKey> table) -> std::optional<Key> {
        for (const QtKey& entry : table) {
            if (entry.qt == qt_key) {
                return entry.key;
            }
        }
        return std::nullopt;
    };
    for (const std::span<const QtKey> table :
         {std::span<const QtKey>(fixed_keys), std::span<const QtKey>(punctuation_keys),
          std::span<const QtKey>(digit_keys)}) {
        if (const auto key = find(table)) {
            return key;
        }
    }
    return std::nullopt;
}

std::optional<core::KeyStep> key_step_from_event(const QKeyEvent& event,
                                                 core::KeyPlatform platform) {
    std::optional<Key> key;
#ifdef Q_OS_LINUX
    if (platform != core::KeyPlatform::macos && platform != core::KeyPlatform::windows &&
        event.nativeScanCode() != 0) {
        key = key_at_scan_code(event.nativeScanCode());
    }
#endif
    if (!key) {
        key = key_from_qt(event.key(), event.modifiers());
    }
    if (!key) {
        return std::nullopt;
    }

    const Qt::KeyboardModifiers held = event.modifiers();
    const bool mac = platform == core::KeyPlatform::macos;
    core::KeyStep step;
    step.key = *key;
    step.modifiers.ctrl = held.testFlag(mac ? Qt::MetaModifier : Qt::ControlModifier);
    step.modifiers.meta = held.testFlag(mac ? Qt::ControlModifier : Qt::MetaModifier);
    step.modifiers.shift = held.testFlag(Qt::ShiftModifier) || event.key() == Qt::Key_Backtab;
    step.modifiers.alt = held.testFlag(Qt::AltModifier);
    // A modifier pressed alone is the key and not a held modifier of itself.
    switch (step.key) {
    case Key::ctrl:
        step.modifiers.ctrl = false;
        break;
    case Key::shift:
        step.modifiers.shift = false;
        break;
    case Key::alt:
        step.modifiers.alt = false;
        break;
    case Key::meta:
        step.modifiers.meta = false;
        break;
    default:
        break;
    }
    return step;
}

} // namespace anvil::ui
