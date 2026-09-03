#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/rendering/rendering.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>

namespace features::esp::item {

	void overlay::on_render( xdraw::draw_list& draw_list )
	{
		const auto& overlay_cfg = settings::g_esp.m_item.m_overlay;

		for ( const auto& item : systems::g_entities.get_by_type( systems::entities::type::item ) )
		{
			const auto info = this->get_info( item );
			if ( !info.valid( ) )
			{
				continue;
			}

			const auto group_id = this->get_item_group( info.schema_hash );

			if ( !overlay_cfg.is_active( group_id ) || info.distance > overlay_cfg.get_group( group_id ).max_distance )
			{
				continue;
			}

			this->add_label( draw_list, info, overlay_cfg.get_group( group_id ) );
		}
	}

	void overlay::add_label( xdraw::draw_list& draw_list, const info& info, const settings::esp::item::overlay::group& cfg )
	{
		const auto screen = systems::g_view.project( info.origin );
		if ( !systems::g_view.projection_valid( screen ) )
		{
			return;
		}

		const auto show_icon = cfg.display == settings::esp::item::overlay::group::display_type::icon || cfg.display == settings::esp::item::overlay::group::display_type::text_and_icon;
		const auto show_text = cfg.display == settings::esp::item::overlay::group::display_type::text || cfg.display == settings::esp::item::overlay::group::display_type::text_and_icon;
		const auto distance_fade = std::clamp( ( cfg.max_distance.value - info.distance ) / std::max( 1.0f, cfg.max_distance.value * 0.35f ), 0.0f, 1.0f );
		const auto icon_color = xdraw::color{ cfg.icon_color.value.r, cfg.icon_color.value.g, cfg.icon_color.value.b, static_cast<std::uint8_t>( cfg.icon_color.value.a * distance_fade ) };
		const auto text_color = xdraw::color{ cfg.text_color.value.r, cfg.text_color.value.g, cfg.text_color.value.b, static_cast<std::uint8_t>( cfg.text_color.value.a * distance_fade ) };
		auto y = screen.y;

		if ( show_icon )
		{
			const auto ico = systems::g_icons.get( info.schema_hash, 0.35f );
			if ( ico && ico->texture )
			{
				const auto iw = static_cast< float >( ico->width );
				const auto ih = static_cast< float >( ico->height );
				const auto ix = std::floorf( screen.x - iw * 0.5f );
				const auto iy = std::floorf( y );

				constexpr auto outline{ xdraw::color{ 0, 0, 0, 255 } };

				draw_list.image( ix - 1.0f, iy, iw, ih, ico->texture.Get( ), outline );
				draw_list.image( ix + 1.0f, iy, iw, ih, ico->texture.Get( ), outline );
				draw_list.image( ix, iy - 1.0f, iw, ih, ico->texture.Get( ), outline );
				draw_list.image( ix, iy + 1.0f, iw, ih, ico->texture.Get( ), outline );
				draw_list.image( ix, iy, iw, ih, ico->texture.Get( ), icon_color );

				y += ih + 1.0f;
			}
		}

		if ( show_text )
		{
			xdraw::push_font( rendering::g_fonts.smallest_pixel7[ rendering::fonts::size::normal ] );

			const auto [w, h] = xdraw::measure_text( info.name );
			draw_list.text( std::floorf( screen.x - w * 0.5f ), std::floorf( y ), info.name, text_color, xdraw::text_style::outlined );

			xdraw::pop_font( );
		}
	}

	overlay::info overlay::get_info( const systems::entities::cached& entity )
	{
		info info{};
		info.entity = entity.ptr;
		info.schema_hash = entity.schema_hash;

		if ( !info.entity )
		{
			return info;
		}

		const auto game_scene_node = memory::read<std::uintptr_t>( info.entity + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
		if ( !game_scene_node )
		{
			return info;
		}

		if ( memory::read<bool>( game_scene_node + SCHEMA( "CGameSceneNode", "m_bDormant"_hash ) ) )
		{
			return info;
		}

		const auto owner_handle = memory::read<std::uint32_t>( info.entity + SCHEMA( "C_BaseEntity", "m_hOwnerEntity"_hash ) );
		if ( owner_handle && owner_handle != 0xffffffff && systems::g_entities.lookup( owner_handle ) )
		{
			return info;
		}

		info.origin = memory::read<math::vector3>( game_scene_node + SCHEMA( "CGameSceneNode", "m_vecAbsOrigin"_hash ) );
		info.distance = systems::g_view.origin( ).distance( info.origin ) * 0.01905f;
		info.vdata = memory::read<std::uintptr_t>( info.entity + SCHEMA( "C_BaseEntity", "m_nSubclassID"_hash ) + 0x8 );

		if ( info.vdata )
		{
			const auto name_ptr = memory::read<const char*>( info.vdata + SCHEMA( "CCSWeaponBaseVData", "m_szName"_hash ) );
			if ( name_ptr ) info.name = memory::read_string( reinterpret_cast<std::uintptr_t>( name_ptr ), 64 );
		}

		if ( info.name.empty( ) )
		{
			static const std::unordered_map<std::uint32_t, const char*> fallback_names
			{
				{ "C_AK47"_hash, "ak-47" }, { "C_WeaponM4A1"_hash, "m4a4" }, { "C_WeaponM4A1Silencer"_hash, "m4a1-s" },
				{ "C_WeaponAWP"_hash, "awp" }, { "C_WeaponSSG08"_hash, "ssg 08" }, { "C_DEagle"_hash, "desert eagle" },
				{ "C_WeaponGlock"_hash, "glock-18" }, { "C_WeaponUSPSilencer"_hash, "usp-s" }, { "C_WeaponP250"_hash, "p250" },
				{ "C_WeaponMAC10"_hash, "mac-10" }, { "C_WeaponMP9"_hash, "mp9" }, { "C_WeaponP90"_hash, "p90" },
				{ "C_WeaponFamas"_hash, "famas" }, { "C_WeaponGalilAR"_hash, "galil ar" }, { "C_WeaponAug"_hash, "aug" },
				{ "C_WeaponSG556"_hash, "sg 553" }, { "C_WeaponNOVA"_hash, "nova" }, { "C_WeaponXM1014"_hash, "xm1014" },
				{ "C_C4"_hash, "c4" }, { "C_WeaponTaser"_hash, "zeus" }, { "C_Knife"_hash, "knife" },
				{ "C_HEGrenade"_hash, "he grenade" }, { "C_Flashbang"_hash, "flashbang" }, { "C_SmokeGrenade"_hash, "smoke" },
				{ "C_MolotovGrenade"_hash, "molotov" }, { "C_IncendiaryGrenade"_hash, "incendiary" }, { "C_DecoyGrenade"_hash, "decoy" }
			};
			if ( const auto it = fallback_names.find( info.schema_hash ); it != fallback_names.end( ) ) info.name = it->second;
		}

		if ( info.name.starts_with( "weapon_" ) )
		{
			info.name.erase( 0, 7 );
		}

		return info;
	}

	std::uint32_t overlay::get_item_group( std::uint32_t schema_hash )
	{
		switch ( schema_hash )
		{
		case "C_DEagle"_hash:
		case "C_WeaponElite"_hash:
		case "C_WeaponFiveSeven"_hash:
		case "C_WeaponGlock"_hash:
		case "C_WeaponHKP2000"_hash:
		case "C_WeaponUSPSilencer"_hash:
		case "C_WeaponP250"_hash:
		case "C_WeaponCZ75a"_hash:
		case "C_WeaponTec9"_hash:
		case "C_WeaponRevolver"_hash:
			return 0;

		case "C_WeaponMAC10"_hash:
		case "C_WeaponMP5SD"_hash:
		case "C_WeaponMP7"_hash:
		case "C_WeaponMP9"_hash:
		case "C_WeaponBizon"_hash:
		case "C_WeaponP90"_hash:
		case "C_WeaponUMP45"_hash:
			return 1;

		case "C_AK47"_hash:
		case "C_WeaponM4A1"_hash:
		case "C_WeaponM4A1Silencer"_hash:
		case "C_WeaponAug"_hash:
		case "C_WeaponFamas"_hash:
		case "C_WeaponGalilAR"_hash:
		case "C_WeaponSG556"_hash:
			return 2;

		case "C_WeaponNOVA"_hash:
		case "C_WeaponSawedoff"_hash:
		case "C_WeaponXM1014"_hash:
		case "C_WeaponMag7"_hash:
			return 3;

		case "C_WeaponAWP"_hash:
		case "C_WeaponG3SG1"_hash:
		case "C_WeaponSCAR20"_hash:
		case "C_WeaponSSG08"_hash:
		case "C_WeaponM249"_hash:
		case "C_WeaponNegev"_hash:
			return 4;

		case "C_HEGrenade"_hash:
		case "C_Flashbang"_hash:
		case "C_SmokeGrenade"_hash:
		case "C_MolotovGrenade"_hash:
		case "C_IncendiaryGrenade"_hash:
		case "C_DecoyGrenade"_hash:
		case "C_C4"_hash:
		case "C_WeaponTaser"_hash:
		case "C_Item_Healthshot"_hash:
		case "C_Knife"_hash:
			return 5;

		default:
			return UINT32_MAX;
		}
	}

} // namespace features::esp::item
