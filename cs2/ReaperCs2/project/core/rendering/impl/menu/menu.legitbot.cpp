// language: C++, file: menu.legitbot.cpp
// improvement: hover tooltips for every legitbot option
// uses xui::tooltip() — fires when the last drawn widget is hovered,
// renders a styled popup with a description string.
//
// xui::tooltip( "text" ) signature:
//   must be called immediately after the widget it annotates.
//   internally it checks xui::ctx().last_item_hovered and, if true,
//   queues a tooltip draw at the end of the frame (so it sits above all
//   other widgets). uses the existing popup style tokens.

#include <pch/pch.hpp>
#include <core/settings.hpp>

#include "../../rendering.hpp"
#include <external/xdraw/xui/xui_tooltip.hpp>

namespace rendering {

namespace detail {

constexpr const char* hitbox_names_legit[]{ "head", "chest", "stomach", "arms", "legs" };

} // namespace detail

void menu::draw_legitbot( float group_w ) const
{
    auto& s  = settings::g_combat;
    auto& lb = s.m_legitbot;
    auto& wg = lb.active_group( this->m_subtab );

    const auto wx        = this->m_x;
    const auto wy        = this->m_y;
    const auto content_x = wx + tokens::gap + tokens::sidebar_w + tokens::gap;
    const auto body_y    = wy + tokens::gap + tokens::subtab_bar_h + tokens::gap;
    const auto content_w = this->m_w - tokens::gap * 2.0f - tokens::sidebar_w - tokens::gap;
    const auto col_w     = ( content_w - tokens::gap ) * 0.5f;
    const auto right_x   = content_x + col_w + tokens::gap;

    xui::layout::set_cursor( content_x - wx, body_y - wy );

    // ── master toggle ────────────────────────────────────────────────────
    if ( xui::begin_child( "##legitbot_master", lb.enabled.value ? col_w : content_w ) )
    {
        xui::checkbox( "enabled", lb.enabled );
        xui::tooltip( "Master switch for the entire legitbot system.\n"
                      "Disabling this stops aimbot, RCS, and triggerbot." );

        if ( lb.enabled.value )
        {
            xui::checkbox( "all weapons", lb.all_weapons );
            xui::tooltip( "Use the same settings for every weapon type instead\n"
                          "of the per-weapon-class groups above." );
        }

        xui::end_child( );
    }

    if ( !lb.enabled.value )
        return;

    // ── aimbot ───────────────────────────────────────────────────────────
    if ( xui::begin_child( "##legitbot_aimbot", col_w ) )
    {
        xui::checkbox( "aimbot", wg.aimbot );
        xui::tooltip( "Smooth aim assist. Moves your crosshair toward the\n"
                      "best visible bone within the configured FOV.\n"
                      "Requires the bind key to be held (default: Mouse5)." );

        xui::slider_float( "fov", wg.fov, 0.5f, 30.0f, "%.1f°" );
        xui::tooltip( "Detection radius in degrees. Only targets whose\n"
                      "bones fall inside this circle are considered.\n"
                      "Lower = more precise, harder to trigger." );

        xui::slider_int( "smooth", wg.smooth, 0, 100, "%d" );
        xui::tooltip( "How gradually the aim moves toward the target.\n"
                      "0 = instant snap. 100 = very slow approach.\n"
                      "Values 15-35 look most human at normal sens." );

        xui::slider_int( "response", wg.response, 10, 100, "%d%%" );
        xui::tooltip( "Controls how quickly assisted movement reaches its\n"
                      "requested speed. Lower values ramp more gradually.\n"
                      "100 gives immediate response without acceleration limiting." );

        xui::slider_float( "deadzone", wg.deadzone, 0.0f, 0.25f, "%.2f°" );
        xui::tooltip( "Stops aim movement inside this angular distance.\n"
                      "This prevents small corrections from oscillating around\n"
                      "a target. Use 0.02-0.06 for normal sensitivity." );

        xui::slider_int( "lead", wg.target_lead, 0, 100, "%d%%" );
        xui::tooltip( "Compensates for moving targets by aiming slightly\n"
                      "ahead in the direction of their velocity.\n"
                      "0 = no lead. 100 = full lead (120 ms lookahead)." );

        xui::slider_int( "sticky", wg.sticky, 0, 100, "%d%%" );
        xui::tooltip( "Target retention strength. Higher values keep the\n"
                      "aim locked on the current target even when a new\n"
                      "one enters the FOV. 0 = always pick closest." );

        xui::checkbox( "humanize", wg.humanize );
        xui::tooltip( "Adds subtle hand-tremor micro-jitter to the smooth\n"
                      "movement path. Makes the motion look more natural\n"
                      "in POV demos and replay analysis." );

        xui::multicombo( "hitboxes", wg.hitboxes, detail::hitbox_names_legit, 5 );
        xui::tooltip( "Which bones to scan for aim targets.\n"
                      "Head only is safest for legit play.\n"
                      "Enabling more hitboxes improves coverage through smokes." );

        xui::checkbox( "visualize fov", wg.visualize_fov );
        xui::tooltip( "Draw the aim FOV as a circle on screen.\n"
                      "Circle moves with RCS compensation when active.\n"
                      "Opens color picker on right-click." );

        if ( xui::begin_popup( "##vfov_popup", 220.0f ) )
        {
            xui::color_picker( "color##vfov", wg.fov_color );
            xui::end_popup( );
        }

        xui::end_child( );
    }

    // ── rcs ──────────────────────────────────────────────────────────────
    if ( xui::begin_child( "##legitbot_rcs", col_w ) )
    {
        xui::checkbox( "recoil control", wg.rcs );
        xui::tooltip( "Compensates for vertical and horizontal recoil\n"
                      "while the aimbot is locked onto a target.\n"
                      "Open the popup to set min/max randomization range." );

        if ( xui::begin_popup( "##rcs_popup", 220.0f ) )
        {
            xui::slider_int( "rcs min", wg.rcs_min, 0, 100, "%d%%" );
            xui::tooltip( "Minimum compensation strength (% of full punch).\n"
                          "Set to 95 to avoid compensating too little." );

            xui::slider_int( "rcs max", wg.rcs_max, 0, 100, "%d%%" );
            xui::tooltip( "Maximum compensation strength (% of full punch).\n"
                          "Keeping this at 105 adds natural variance." );

            xui::end_popup( );
        }

        xui::checkbox( "standalone rcs", wg.standalone_rcs );
        xui::tooltip( "Apply recoil control even without an aimbot target.\n"
                      "Useful for spray-controlling manually.\n"
                      "Correction is spread over 2 ticks for smoothness." );

        if ( xui::begin_popup( "##src_popup", 220.0f ) )
        {
            xui::slider_int( "strength", wg.standalone_rcs_strength, 0, 100, "%d%%" );
            xui::tooltip( "How much of the total punch to correct.\n"
                          "100 = full correction. 50 = half-compensation." );

            xui::slider_int( "min##src", wg.standalone_rcs_min, 0, 100, "%d%%" );
            xui::tooltip( "Lower bound of the random compensation range." );

            xui::slider_int( "max##src", wg.standalone_rcs_max, 0, 100, "%d%%" );
            xui::tooltip( "Upper bound of the random compensation range." );

            xui::end_popup( );
        }

        xui::end_child( );
    }

    xui::layout::set_cursor( right_x - wx, body_y - wy );

    // ── triggerbot ───────────────────────────────────────────────────────
    if ( xui::begin_child( "##legitbot_triggerbot", col_w ) )
    {
        xui::checkbox( "triggerbot", wg.triggerbot );
        xui::tooltip( "Automatically fires when an enemy hitbox is under\n"
                      "your crosshair. Works with spread prediction (seeded mode)\n"
                      "or an exact unseeded crosshair ray." );

        xui::slider_int( "delay", wg.trigger_delay, 0, 250, "%d ms" );
        xui::tooltip( "Milliseconds to wait after detecting a target\n"
                      "before firing. Simulates human reaction time.\n"
                      "10-30 ms looks natural, 0 ms is instant." );

        xui::slider_int( "hitchance", wg.trigger_hitchance, 0, 100, "%d%%" );
        xui::tooltip( "Minimum probability required in unseeded mode.\n"
                      "Seeded mode validates the exact predicted bullet path." );

        xui::slider_int( "min damage", wg.trigger_min_damage, 1, 125, "%d" );
        xui::tooltip( "Minimum expected damage for every triggerbot shot.\n"
                      "Values over 100 are health-relative: 101 = HP + 1." );

        xui::checkbox( "head only", wg.trigger_head_only );
        xui::tooltip( "Restrict triggerbot to fire only when the crosshair\n"
                      "is directly over the head hitbox.\n"
                      "Safest against anti-cheat spray detection." );

        xui::checkbox( "seeded", wg.give_me_your_seed );
        xui::tooltip( "Use spread seed prediction to calculate the exact\n"
                      "bullet path for this tick and check capsule intersection.\n"
                      "Autowall and reaction delay still apply." );

        xui::end_child( );
    }

    // ── autowall ─────────────────────────────────────────────────────────
    if ( xui::begin_child( "##legitbot_other", col_w ) )
    {
        xui::checkbox( "autowall", wg.autowall );
        xui::tooltip( "Allow the aimbot and triggerbot to target enemies\n"
                      "through penetrable surfaces (walls, floors, doors).\n"
                      "Opens min damage config on right-click." );

        if ( xui::begin_popup( "##aw_popup", 220.0f ) )
        {
            xui::slider_int( "min damage##aw", wg.min_damage, 1, 125, "%d" );
            xui::tooltip( "Minimum damage required after wall penetration\n"
                          "for the aimbot/trigger to fire through the surface.\n"
                          "Values over 100 are health-relative: 101 = HP + 1." );

            xui::end_popup( );
        }

        xui::end_child( );
    }
}

} // namespace rendering
