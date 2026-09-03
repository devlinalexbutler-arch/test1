#include <pch/pch.hpp>
#include <core/settings.hpp>

#include "../../rendering.hpp"

namespace rendering {

	namespace detail {

		constexpr const char* k_cham_material_names[ ]{
			"liquid", "metallic", "matte", "flat", "bloom", "outlines", "glow", "electric", "distortion", "hologram", "pearl",
			"liquid (iz)", "matte (iz)", "flat (iz)", "bloom (iz)", "outlines (iz)", "glow (iz)", "distortion (iz)", "hologram (iz)"
		};
		constexpr auto k_cham_material_count = static_cast< int >( settings::esp::cham_ids::count );

		inline static void draw_chams_layer( const char* label, const char* popup_id, settings::esp::chams_layer& layer )
		{
			xui::checkbox( label, layer.enabled );
			xui::tooltip( "Toggle this chams layer.\n"
			              "Right-click to pick material and color." );
			if ( xui::begin_popup( popup_id, 220.0f ) )
			{
				xui::combo( "material", layer.material.value, k_cham_material_names, k_cham_material_count );
				xui::tooltip( "Shader used for this layer.\n"
				              "(iz) variants ignore Z and draw through walls." );

				xui::color_picker( "color", layer.color );
				xui::tooltip( "Tint applied to this chams layer." );
				xui::end_popup( );
			}
		}

		inline static void draw_chams_config( const char* label, const char* id_suffix, settings::esp::chams_config& cfg, bool show_overlay = true )
		{
			xui::checkbox( label, cfg.enabled );
			xui::tooltip( "Master toggle for this chams set.\n"
			              "Enable layers below to actually draw." );

			char label_buf[ 64 ]{};
			char popup_id[ 64 ]{};

			std::snprintf( label_buf, sizeof( label_buf ), "visible##%s", id_suffix );
			std::snprintf( popup_id, sizeof( popup_id ), "##primary_%s", id_suffix );
			draw_chams_layer( label_buf, popup_id, cfg.primary );

			std::snprintf( label_buf, sizeof( label_buf ), "non visible##%s", id_suffix );
			std::snprintf( popup_id, sizeof( popup_id ), "##secondary_%s", id_suffix );
			draw_chams_layer( label_buf, popup_id, cfg.secondary );

			if ( show_overlay )
			{
				std::snprintf( label_buf, sizeof( label_buf ), "overlay layer##%s", id_suffix );
				std::snprintf( popup_id, sizeof( popup_id ), "##overlay_%s", id_suffix );
				draw_chams_layer( label_buf, popup_id, cfg.overlay );
			}
		}

	} // namespace detail

	void menu::draw_player( float group_w ) const
	{
		auto& esp = settings::g_esp;
		auto& p = esp.m_player;

		const auto col_w = ( this->m_body_w - tokens::gap ) * 0.5f;
		const auto subtab = this->m_subtab;
		const auto has_overlay = ( subtab <= 1 );

		auto& glow = (subtab == 0) ? p.m_glow.enemy : p.m_glow.team;
		auto& glow_ragdoll = (subtab == 0) ? p.m_glow.enemy_ragdoll : p.m_glow.team_ragdoll;

		xui::layout::set_cursor( this->m_body_x - this->m_x, this->m_body_y - this->m_y );

		if ( has_overlay )
		{
			auto& ov = p.m_overlay[ subtab ];

			if ( xui::begin_child( "##player_esp", col_w ) )
			{
				xui::checkbox( "esp overlay", ov.enabled );
				xui::tooltip( "Master switch for 2D player overlay ESP\n"
				              "on this team (boxes, bars, names, flags)." );

				xui::checkbox( "bounding box", ov.m_box.enabled );
				xui::tooltip( "Draw a 2D box around the player.\n"
				              "Right-click for style, fill, and colors." );
				if ( xui::begin_popup( "##box_popup", 220.0f ) )
				{
					constexpr const char* box_styles[ ]{ "full", "cornered" };
					xui::combo( "style##box", ov.m_box.style.value, box_styles, 2 );
					xui::tooltip( "Full box draws all four sides.\n"
					              "Cornered only draws the corners." );

					xui::checkbox( "fill", ov.m_box.fill );
					xui::tooltip( "Fill the box interior with a translucent color." );

					xui::checkbox( "outline", ov.m_box.outline );
					xui::tooltip( "Add a 1px outline for contrast on bright maps." );

					xui::slider_float( "corner length", ov.m_box.corner_length, 2.0f, 20.0f, "%.0f" );
					xui::tooltip( "Length of each corner segment in cornered mode." );

					xui::color_picker( "visible color##box", ov.m_box.visible_color );
					xui::tooltip( "Box color when the player is visible." );

					xui::color_picker( "occluded color##box", ov.m_box.occluded_color );
					xui::tooltip( "Box color when the player is behind cover." );
					xui::end_popup( );
				}

				xui::checkbox( "skeleton", ov.m_skeleton.enabled );
				xui::tooltip( "Draw bone lines through the player model.\n"
				              "Right-click for mode, thickness, and colors." );
				if ( xui::begin_popup( "##skeleton_popup", 220.0f ) )
				{
					constexpr const char* skel_modes[ ]{ "normal", "backtrack" };
					xui::combo( "mode##skel", ov.m_skeleton.type.value, skel_modes, 2 );
					xui::tooltip( "Normal uses the current pose.\n"
					              "Backtrack draws recorded history bones." );

					xui::slider_float( "thickness##skel", ov.m_skeleton.thickness, 0.5f, 4.0f, "%.1f" );
					xui::tooltip( "Line thickness of the skeleton." );

					xui::color_picker( "visible color##skel", ov.m_skeleton.visible_color );
					xui::tooltip( "Bone color when the player is visible." );

					xui::color_picker( "occluded color##skel", ov.m_skeleton.occluded_color );
					xui::tooltip( "Bone color when the player is behind cover." );
					xui::end_popup( );
				}

				xui::checkbox( "health bar", ov.m_health_bar.enabled );
				xui::tooltip( "Show remaining health as a bar on the box.\n"
				              "Right-click for position, gradient, and colors." );
				if ( xui::begin_popup( "##health_popup", 220.0f ) )
				{
					constexpr const char* bar_positions[ ]{ "left", "top", "bottom" };
					xui::combo( "position##hp", ov.m_health_bar.position.value, bar_positions, 3 );

					xui::checkbox( "outline##hp", ov.m_health_bar.outline_setting );
					xui::checkbox( "gradient##hp", ov.m_health_bar.gradient );
					xui::checkbox( "show value##hp", ov.m_health_bar.show_value );
					xui::checkbox( "glow##hp", ov.m_health_bar.glow );
					xui::color_picker( "full color##hp", ov.m_health_bar.full_color );
					xui::color_picker( "low color##hp", ov.m_health_bar.low_color );
					xui::color_picker( "background##hp", ov.m_health_bar.background_color );
					xui::color_picker( "outline color##hp", ov.m_health_bar.outline_color );
					xui::color_picker( "text color##hp", ov.m_health_bar.text_color );
					xui::color_picker( "glow color##hp", ov.m_health_bar.glow_color );
					xui::slider_float( "glow strength##hp", ov.m_health_bar.glow_strength, 0.1f, 1.0f, "%.2f" );
					xui::end_popup( );
				}

				xui::checkbox( "ammo bar", ov.m_ammo_bar.enabled );
				xui::tooltip( "Show magazine ammo as a bar on the box.\n"
				              "Right-click for position and colors." );
				if ( xui::begin_popup( "##ammo_popup", 220.0f ) )
				{
					constexpr const char* bar_positions[ ]{ "left", "top", "bottom" };
					xui::combo( "position##ammo", ov.m_ammo_bar.position.value, bar_positions, 3 );

					xui::checkbox( "outline##ammo", ov.m_ammo_bar.outline_setting );
					xui::checkbox( "gradient##ammo", ov.m_ammo_bar.gradient );
					xui::checkbox( "show value##ammo", ov.m_ammo_bar.show_value );
					xui::checkbox( "glow##ammo", ov.m_ammo_bar.glow );
					xui::color_picker( "full color##ammo", ov.m_ammo_bar.full_color );
					xui::color_picker( "low color##ammo", ov.m_ammo_bar.low_color );
					xui::color_picker( "background##ammo", ov.m_ammo_bar.background_color );
					xui::color_picker( "outline color##ammo", ov.m_ammo_bar.outline_color );
					xui::color_picker( "text color##ammo", ov.m_ammo_bar.text_color );
					xui::color_picker( "glow color##ammo", ov.m_ammo_bar.glow_color );
					xui::slider_float( "glow strength##ammo", ov.m_ammo_bar.glow_strength, 0.1f, 1.0f, "%.2f" );
					xui::end_popup( );
				}

				xui::checkbox( "name", ov.m_name.enabled );
				xui::tooltip( "Draw the player's name above the box.\n"
				              "Right-click to change color." );
				if ( xui::begin_popup( "##name_popup", 220.0f ) )
				{
					xui::color_picker( "color##name", ov.m_name.color );
					xui::end_popup( );
				}

				xui::checkbox( "weapon", ov.m_weapon.enabled );
				xui::tooltip( "Show the active weapon as text, icon, or both.\n"
				              "Right-click for display mode and colors." );
				if ( xui::begin_popup( "##weapon_popup", 220.0f ) )
				{
					constexpr const char* display_types[ ]{ "text", "icon", "text + icon" };
					xui::combo( "display##wep", ov.m_weapon.display.value, display_types, 3 );

					xui::color_picker( "text color##wep", ov.m_weapon.text_color );
					xui::color_picker( "icon color##wep", ov.m_weapon.icon_color );
					xui::end_popup( );
				}

				xui::checkbox( "info flags", ov.m_info_flags.enabled );
				xui::tooltip( "Extra status tags next to the box: money, armor,\n"
				              "kit, scoped, defusing, flashed, ping, distance." );
				if ( xui::begin_popup( "##flags_popup", 220.0f ) )
				{
					constexpr const char* flag_names[ ]{ "money", "armor", "kit", "scoped", "defusing", "flashed", "ping", "distance" };
					xui::multicombo( "flags##mc", ov.m_info_flags.flags, flag_names, settings::esp::player::overlay::info_flags::count );

					xui::color_picker( "money##flags", ov.m_info_flags.money_color );
					xui::color_picker( "armor##flags", ov.m_info_flags.armor_color );
					xui::color_picker( "kit##flags", ov.m_info_flags.kit_color );
					xui::color_picker( "scoped##flags", ov.m_info_flags.scoped_color );
					xui::color_picker( "defusing##flags", ov.m_info_flags.defusing_color );
					xui::color_picker( "flashed##flags", ov.m_info_flags.flashed_color );
					xui::color_picker( "distance##flags", ov.m_info_flags.distance_color );
					xui::end_popup( );
				}

				xui::checkbox( "oof arrows", ov.m_oof_arrow.enabled );
				xui::tooltip( "Off-screen arrows pointing at players outside FOV.\n"
				              "Right-click for size, radius, and colors." );
				if ( xui::begin_popup( "##oof_popup", 220.0f ) )
				{
					xui::checkbox( "glow##oof", ov.m_oof_arrow.glow );
					xui::slider_float( "width##oof", ov.m_oof_arrow.width, 4.0f, 40.0f, "%.0f" );
					xui::slider_float( "height##oof", ov.m_oof_arrow.height, 4.0f, 40.0f, "%.0f" );
					xui::slider_float( "radius x##oof", ov.m_oof_arrow.radius_x, 50.0f, 600.0f, "%.0f" );
					xui::slider_float( "radius y##oof", ov.m_oof_arrow.radius_y, 50.0f, 600.0f, "%.0f" );
					xui::slider_float( "glow strength##oof", ov.m_oof_arrow.glow_strength, 0.1f, 1.0f, "%.2f" );
					xui::color_picker( "visible color##oof", ov.m_oof_arrow.visible_color );
					xui::color_picker( "occluded color##oof", ov.m_oof_arrow.occluded_color );
					xui::end_popup( );
				}


				xui::end_child ();

				if (xui::begin_child ("##player_glow", col_w)) {
					xui::checkbox ("glow", glow.enabled);
					xui::tooltip( "Engine glow outline around living players.\n"
					              "Right-click to pick the color." );
					if (xui::begin_popup ("##glow_popup", 220.0f)) {
						xui::color_picker ("color##glow", glow.color);
						xui::end_popup ();
					}

					xui::checkbox ("ragdoll glow", glow_ragdoll.enabled);
					xui::tooltip( "Keep glow on the corpse after the player dies." );
					if (xui::begin_popup ("##glow_rag_popup", 220.0f)) {
						xui::color_picker ("color##glow_rag", glow_ragdoll.color);
						xui::end_popup ();
					}

					xui::end_child ();
				}


			}
		}
		else
		{
			if ( xui::begin_child( "##local_chams_glow", col_w ) )
			{
				detail::draw_chams_config( "chams", "local_main", p.m_chams.local );

				xui::layout::separator( );

				xui::checkbox( "lower opacity", esp.m_local_alpha.enabled );
				xui::tooltip( "Fade your own model so it does not block view.\n"
				              "Right-click for opacity and scoped-only." );
				if ( xui::begin_popup( "##local_alpha_popup", 220.0f ) )
				{
					xui::slider_float( "opacity", esp.m_local_alpha.opacity, 0.0f, 1.0f, "%.2f" );
					xui::checkbox( "only when scoped", esp.m_local_alpha.only_scoped );
					xui::end_popup( );
				}

				xui::layout::separator( );

				detail::draw_chams_config( "ragdoll chams", "local_ragdoll", p.m_chams.local_ragdoll, false );

				xui::layout::separator( );

				xui::checkbox( "glow", p.m_glow.local.enabled );
				xui::tooltip( "Engine glow on your own player model." );
				if ( xui::begin_popup( "##local_glow_popup", 220.0f ) )
				{
					xui::color_picker( "color##local_glow", p.m_glow.local.color );
					xui::end_popup( );
				}

				xui::checkbox( "ragdoll glow", p.m_glow.local_ragdoll.enabled );
				xui::tooltip( "Glow on your ragdoll after you die." );
				if ( xui::begin_popup( "##local_glow_rag_popup", 220.0f ) )
				{
					xui::color_picker( "color##local_glow_rag", p.m_glow.local_ragdoll.color );
					xui::end_popup( );
				}

				xui::end_child( );
			}
		}

		xui::layout::set_cursor( this->m_body_x - this->m_x + col_w + tokens::gap, this->m_body_y - this->m_y );

		if ( has_overlay )
		{
			auto& chams = ( subtab == 0 ) ? p.m_chams.enemy : p.m_chams.team;
			auto& chams_ragdoll = ( subtab == 0 ) ? p.m_chams.enemy_ragdoll : p.m_chams.team_ragdoll;
			auto& glow = ( subtab == 0 ) ? p.m_glow.enemy : p.m_glow.team;
			auto& glow_ragdoll = ( subtab == 0 ) ? p.m_glow.enemy_ragdoll : p.m_glow.team_ragdoll;

			if ( xui::begin_child( "##player_chams", col_w ) )
			{
				detail::draw_chams_config( "chams", "main", chams );

				xui::layout::separator( );

				detail::draw_chams_config( "ragdoll chams", "ragdoll", chams_ragdoll, false );

				if ( subtab == 0 )
				{
					xui::layout::separator( );

					detail::draw_chams_config( "backtrack chams", "bt", p.m_chams.backtrack, false );
/*					xui::layout::separator (); not enough menu space with this*/
					detail::draw_chams_config ("onshot chams", "os", p.m_chams.onshot, false);

					xui::slider_float ("fade##ft", p.m_chams.onshot_fade_time, 0.1f, 5.0f, "%.0f");
					xui::tooltip( "How long on-shot chams linger after a fire, in seconds." );

				}

				xui::end_child( );
			}

	}
	else
		{
			if ( xui::begin_child( "##viewmodel", col_w ) )
			{
				detail::draw_chams_config( "weapon chams", "vm_weapon", esp.m_viewmodel.weapon );

				xui::layout::separator( );

				detail::draw_chams_config( "arms chams", "vm_arms", esp.m_viewmodel.arms );

				xui::end_child( );
			}
		}
	}

} // namespace rendering