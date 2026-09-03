// file: xui_tooltip.hpp
// Adds xui::tooltip() — call it immediately after any xui widget to attach
// a hover description. When the mouse dwells on the last-drawn item for
// k_delay seconds, a styled popup appears near the cursor.
//
// HOW TO INTEGRATE:
//   #include "xui_tooltip.hpp" after xui.hpp (already in pch.hpp).
//   Call xui::tooltip_impl::flush() at the end of menu::draw() before xui::end().
//
// USAGE:
//   xui::checkbox( "aimbot", wg.aimbot );
//   xui::tooltip( "Smooth aim assist description..." );

#pragma once
#include "../xdraw.hpp"
#include "xui.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace xui {

namespace detail {

struct tooltip_state
{
	std::string pending_text{};
	rect        anchor{};
	bool        touched{};

	std::string visible_text{};
	float       show_anim{};
	float       dwell_time{};
};

inline tooltip_state& g_tooltip( )
{
	static tooltip_state s{};
	return s;
}

} // namespace detail

inline void tooltip( std::string_view text )
{
	auto& s         = detail::g_tooltip( );
	auto& c         = xui::ctx( );
	const auto item = c.last_item_rect;

	if ( item.w <= 0.0f || item.h <= 0.0f )
		return;

	if ( c.input.in_rect( item ) )
	{
		s.pending_text = std::string( text );
		s.anchor       = item;
		s.touched      = true;
	}
}

namespace tooltip_impl {

constexpr float k_delay      = 0.32f;
constexpr float k_pad_x      = 12.0f;
constexpr float k_pad_y      = 8.0f;
constexpr float k_line_gap   = 3.0f;
constexpr float k_rounding   = 6.0f;
constexpr float k_max_width  = 340.0f;
constexpr float k_fade_speed = 14.0f;

inline std::vector<std::string> wrap_lines( std::string_view text, float max_w )
{
	std::vector<std::string> out;

	auto push_wrapped = [ & ]( std::string_view paragraph )
	{
		if ( paragraph.empty( ) )
		{
			out.emplace_back( );
			return;
		}

		std::string line{};
		std::size_t i = 0;
		while ( i < paragraph.size( ) )
		{
			while ( i < paragraph.size( ) && paragraph[ i ] == ' ' )
				++i;
			if ( i >= paragraph.size( ) )
				break;

			std::size_t end = i;
			while ( end < paragraph.size( ) && paragraph[ end ] != ' ' )
				++end;

			const auto word = std::string( paragraph.substr( i, end - i ) );
			const auto trial = line.empty( ) ? word : line + " " + word;
			const auto [ tw, th ] = xdraw::measure_text( trial.c_str( ) );
			( void )th;

			if ( tw <= max_w || line.empty( ) )
			{
				line = trial;
			}
			else
			{
				out.push_back( line );
				line = word;
			}
			i = end;
		}

		if ( !line.empty( ) )
			out.push_back( line );
	};

	std::size_t start = 0;
	for ( std::size_t i = 0; i <= text.size( ); ++i )
	{
		if ( i == text.size( ) || text[ i ] == '\n' )
		{
			push_wrapped( text.substr( start, i - start ) );
			start = i + 1;
		}
	}

	return out;
}

inline void flush( )
{
	auto& s   = detail::g_tooltip( );
	auto& c   = xui::ctx( );
	const auto dt = xdraw::delta_time( );

	const bool hovering = s.touched && !s.pending_text.empty( );
	s.touched = false;

	if ( hovering )
	{
		s.dwell_time += dt;
		if ( s.dwell_time >= k_delay )
			s.visible_text = s.pending_text;
	}
	else
	{
		s.dwell_time = 0.0f;
		s.pending_text.clear( );
	}

	const bool should_show = hovering
		&& s.dwell_time >= k_delay
		&& !s.visible_text.empty( );

	const auto target_anim = should_show ? 1.0f : 0.0f;
	s.show_anim += ( target_anim - s.show_anim ) * std::min( k_fade_speed * dt, 1.0f );

	if ( s.show_anim < 0.01f )
	{
		if ( !should_show )
			s.visible_text.clear( );
		return;
	}

	const auto lines = wrap_lines( s.visible_text, k_max_width );

	float text_w = 0.0f;
	float text_h = 0.0f;
	for ( std::size_t i = 0; i < lines.size( ); ++i )
	{
		const auto [ lw, lh ] = xdraw::measure_text( lines[ i ].c_str( ) );
		text_w = std::max( text_w, lw );
		text_h += lh + ( i + 1 < lines.size( ) ? k_line_gap : 0.0f );
	}

	const float popup_w = text_w + k_pad_x * 2.0f;
	const float popup_h = text_h + k_pad_y * 2.0f;

	const auto [ vw, vh ] = xdraw::viewport_size( );
	const auto sw = static_cast<float>( vw );
	const auto sh = static_cast<float>( vh );

	const auto& inp = c.input;
	float px = inp.mouse_x + 16.0f;
	float py = inp.mouse_y + 16.0f;

	if ( px + popup_w > sw - 6.0f )
		px = std::max( 6.0f, inp.mouse_x - popup_w - 8.0f );
	if ( py + popup_h > sh - 6.0f )
		py = std::max( 6.0f, inp.mouse_y - popup_h - 8.0f );

	const auto alpha = static_cast<std::uint8_t>( 255.0f * s.show_anim );
	auto& dl = xdraw::get( xdraw::layer::top );

	auto& glow = xdraw::get_glow( );
	glow.rect( px - 2.0f, py - 2.0f, popup_w + 4.0f, popup_h + 4.0f,
	           xdraw::color{ 0, 0, 0, static_cast<std::uint8_t>( 80 * s.show_anim ) },
	           xdraw::corner_radius{ k_rounding }, 2.0f );

	dl.rect_filled_blurred( px, py, popup_w, popup_h,
	                        xdraw::corner_radius{ k_rounding },
	                        xdraw::color{ 255, 255, 255, static_cast<std::uint8_t>( 60 * s.show_anim ) } );

	dl.rect_filled( px, py, popup_w, popup_h,
	                xdraw::color{ 12, 14, 18, alpha },
	                xdraw::corner_radius{ k_rounding } );

	dl.rect( px, py, popup_w, popup_h,
	         xdraw::color{ 60, 80, 65, static_cast<std::uint8_t>( 160 * s.show_anim ) },
	         xdraw::corner_radius{ k_rounding }, 1.0f );

	float ty = py + k_pad_y;
	for ( const auto& line : lines )
	{
		const auto [ lw, lh ] = xdraw::measure_text( line.c_str( ) );
		dl.text( px + k_pad_x, ty, line.c_str( ), xdraw::color{ 195, 215, 200, alpha } );
		ty += lh + k_line_gap;
	}
}

} // namespace tooltip_impl

} // namespace xui
