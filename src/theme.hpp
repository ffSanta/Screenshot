#pragma once

#include <cairo.h>

namespace ss::theme {

struct Color { double r, g, b; };

// Catppuccin Mocha - dark enough to sit over anything without glare, with a
// blue accent that stays visible against both light and dark desktops.
inline constexpr Color base     { 0.118, 0.118, 0.180 };  // frame body
inline constexpr Color mantle   { 0.094, 0.094, 0.145 };  // toolbar body
inline constexpr Color crust    { 0.067, 0.067, 0.106 };  // text on bright buttons
inline constexpr Color text     { 0.804, 0.839, 0.957 };
inline constexpr Color subtext  { 0.651, 0.678, 0.784 };
inline constexpr Color surface1 { 0.271, 0.278, 0.353 };
inline constexpr Color surface2 { 0.345, 0.357, 0.439 };
inline constexpr Color blue     { 0.537, 0.706, 0.980 };  // accent / handles
inline constexpr Color sky      { 0.537, 0.863, 0.922 };  // accent, hovered
inline constexpr Color green    { 0.651, 0.890, 0.631 };  // Save
inline constexpr Color teal     { 0.580, 0.886, 0.835 };  // Save, hovered

inline void set(cairo_t* cr, Color c) { cairo_set_source_rgb(cr, c.r, c.g, c.b); }

inline void roundedRect(cairo_t* cr, double x, double y,
                        double w, double h, double r) {
    constexpr double kPi = 3.14159265358979323846;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -kPi / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,         kPi / 2);
    cairo_arc(cr, x + r,     y + h - r, r, kPi / 2,   kPi);
    cairo_arc(cr, x + r,     y + r,     r, kPi,       3 * kPi / 2);
    cairo_close_path(cr);
}

} // namespace ss::theme
