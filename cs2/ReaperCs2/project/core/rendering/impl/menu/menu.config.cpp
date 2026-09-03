#include <pch/pch.hpp>
#include <utilities/math/math.hpp>
#include <core/settings.hpp>
#include <utilities/diag.hpp>

#include "../../rendering.hpp"

namespace rendering {

	namespace detail {

		std::string search_buf{};
		std::string name_buf{};
		std::string status_msg{};
		float status_timer{ 0.0f };
		std::vector<std::wstring> config_list{};
		auto selected{ -1 };
		auto needs_refresh{ true };
		auto confirm_delete{ false };
		auto confirm_reset{ false };
		auto confirm_timer{ 0.0f };
		std::wstring last_diagnostic_path{};

		static bool create_diagnostic_package( )
		{
			wchar_t temp_path[ MAX_PATH ]{};
			if ( !GetTempPathW( MAX_PATH, temp_path ) )
			{
				return false;
			}

			SYSTEMTIME st{};
			GetLocalTime( &st );
			wchar_t folder_name[ 96 ]{};
			std::swprintf( folder_name, std::size( folder_name ), L"velocity_diagnostics_%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond );
			last_diagnostic_path = std::filesystem::path( temp_path ) / folder_name;

			std::error_code ec{};
			std::filesystem::create_directories( last_diagnostic_path, ec );
			if ( ec )
			{
				return false;
			}

			diag::write( diag::level::info, "manual diagnostic package requested" );
			if ( diag::g_log_file )
			{
				FlushFileBuffers( diag::g_log_file );
			}
			diag::capture_snapshot( "manual diagnostic package" );

			auto copy_artifact = [ & ]( const wchar_t* source, const wchar_t* name )
			{
				if ( source && *source && std::filesystem::exists( source, ec ) )
				{
					std::filesystem::copy_file( source, std::filesystem::path( last_diagnostic_path ) / name, std::filesystem::copy_options::overwrite_existing, ec );
					ec.clear( );
				}
			};

			copy_artifact( diag::g_log_path, L"velocity_init.log" );
			copy_artifact( diag::g_previous_log_path, L"velocity_init.previous.log" );
			copy_artifact( diag::g_dump_path, L"velocity_snapshot.dmp" );
			copy_artifact( diag::g_previous_dump_path, L"velocity_crash.previous.dmp" );

			SYSTEM_INFO system_info{};
			GetNativeSystemInfo( &system_info );
			MEMORYSTATUSEX memory_status{ sizeof( MEMORYSTATUSEX ) };
			GlobalMemoryStatusEx( &memory_status );

			std::ofstream report( std::filesystem::path( last_diagnostic_path ) / "system.txt", std::ios::binary | std::ios::trunc );
			if ( !report )
			{
				return false;
			}
			report << "velocity diagnostic package\r\n";
			report << "build: " << __DATE__ << " " << __TIME__ << "\r\n";
			report << "configuration: development x64\r\n";
			report << "process id: " << GetCurrentProcessId( ) << "\r\n";
			report << "processor architecture: " << system_info.wProcessorArchitecture << "\r\n";
			report << "logical processors: " << system_info.dwNumberOfProcessors << "\r\n";
			report << "memory load: " << memory_status.dwMemoryLoad << "%\r\n";
			report << "physical memory mb: " << memory_status.ullTotalPhys / ( 1024ull * 1024ull ) << "\r\n";
			report << "available memory mb: " << memory_status.ullAvailPhys / ( 1024ull * 1024ull ) << "\r\n";
			report << "module base: 0x" << std::hex << reinterpret_cast<std::uintptr_t>( diag::g_module ) << "\r\n";
			report << "module end: 0x" << diag::g_module_end << "\r\n";
			return true;
		}

		static inline bool copy_to_clipboard( const std::string& text )
		{
			if ( !OpenClipboard( nullptr ) )
			{
				return false;
			}

			EmptyClipboard( );

			const auto size = ( text.size( ) + 1 ) * sizeof( char );

			auto mem = GlobalAlloc( GMEM_MOVEABLE, size );
			if ( !mem )
			{
				CloseClipboard( );
				return false;
			}

			auto dest = GlobalLock( mem );
			if ( dest )
			{
				std::memcpy( dest, text.c_str( ), size );
				GlobalUnlock( mem );
			}

			SetClipboardData( CF_TEXT, mem );
			CloseClipboard( );
			return true;
		}

		static inline std::string paste_from_clipboard( )
		{
			if ( !OpenClipboard( nullptr ) )
			{
				return {};
			}

			std::string result{};

			auto mem = GetClipboardData( CF_TEXT );
			if ( mem )
			{
				auto data = static_cast< const char* >( GlobalLock( mem ) );
				if ( data )
				{
					result = data;
					GlobalUnlock( mem );
				}
			}

			CloseClipboard( );
			return result;
		}

		static inline void wide_to_utf8( const std::wstring& wide, char* out, int out_size )
		{
			WideCharToMultiByte( CP_UTF8, 0, wide.c_str( ), -1, out, out_size, nullptr, nullptr );
		}

		static inline std::wstring utf8_to_wide( const std::string& utf8 )
		{
			wchar_t buf[ 128 ]{};
			MultiByteToWideChar( CP_UTF8, 0, utf8.c_str( ), -1, buf, 128 );
			return buf;
		}

		static inline bool config_matches_search( const std::wstring& wname )
		{
			if ( detail::search_buf.empty( ) )
			{
				return true;
			}

			char narrow[ 128 ]{};
			wide_to_utf8( wname, narrow, sizeof( narrow ) );

			std::string lower_name{ narrow };
			std::string lower_search{ detail::search_buf };

			for ( auto& c : lower_name )
			{
				c = static_cast< char >( std::tolower( c ) );
			}

			for ( auto& c : lower_search )
			{
				c = static_cast< char >( std::tolower( c ) );
			}

			return lower_name.find( lower_search ) != std::string::npos;
		}

		static inline std::string selected_name( )
		{
			if ( detail::selected < 0 || detail::selected >= static_cast< int >( detail::config_list.size( ) ) )
			{
				return {};
			}

			char narrow[ 128 ]{};
			wide_to_utf8( detail::config_list[ detail::selected ], narrow, sizeof( narrow ) );
			return narrow;
		}

		static inline void reset_defaults( )
		{
			auto& reg = config::detail::get_registry( );

			for ( auto& f : reg.fields )
			{
				char key_str[ 12 ];
				std::snprintf( key_str, sizeof( key_str ), "%08x", f.key );

				auto def = reg.defaults.find( key_str );
				if ( def != reg.defaults.end( ) )
				{
					config::serial::json_to_field( *def, f );
				}
			}

			settings::finalize_binds( );
		}

	} // namespace detail

	void menu::draw_config( float group_w )
	{
		( void )group_w;

		if ( detail::needs_refresh )
		{
			detail::config_list = config::registry::list( );
			detail::needs_refresh = false;

			if ( detail::selected >= static_cast< int >( detail::config_list.size( ) ) )
			{
				detail::selected = -1;
			}

			// Prefer selecting by explicit name buffer, then search buffer
			if ( detail::selected < 0 )
			{
				const auto want_src = !detail::name_buf.empty( ) ? detail::name_buf : detail::search_buf;
				if ( !want_src.empty( ) )
				{
					const auto want = detail::utf8_to_wide( want_src );
					for ( auto i = 0; i < static_cast< int >( detail::config_list.size( ) ); ++i )
					{
						if ( detail::config_list[ static_cast< std::size_t >( i ) ] == want )
						{
							detail::selected = i;
							break;
						}
					}
				}
			}
		}

		const auto dt = xdraw::delta_time( );

		if ( detail::status_timer > 0.0f )
		{
			detail::status_timer -= dt;
			if ( detail::status_timer <= 0.0f )
			{
				detail::status_msg.clear( );
			}
		}

		if ( detail::confirm_delete || detail::confirm_reset )
		{
			detail::confirm_timer += dt;

			if ( detail::confirm_timer > 3.0f )
			{
				detail::confirm_delete = false;
				detail::confirm_reset = false;
			}
		}

		auto& dl = xui::draw::current( );
		const auto& s = xui::ctx( ).style;
		const auto& input = xui::ctx( ).input;

		const auto wx = this->m_x;
		const auto wy = this->m_y;
		const auto content_x = wx + tokens::gap + tokens::sidebar_w + tokens::gap;
		const auto body_y = wy + tokens::gap + tokens::subtab_bar_h + tokens::gap;
		const auto content_w = this->m_w - tokens::gap * 2.0f - tokens::sidebar_w - tokens::gap;
		const auto body_h = this->m_h - tokens::gap * 2.0f - tokens::subtab_bar_h - tokens::gap;

		xui::layout::set_cursor( content_x - wx, body_y - wy );

		if ( !xui::begin_child( "##cfg_panel", content_w, body_h, false ) )
		{
			return;
		}

		static constexpr const char* theme_presets[ ]{ "blue", "cyan", "pink", "green", "midnight" };
		auto& theme = settings::g_misc.m_menu_theme;
		if ( xui::combo( "theme preset", theme.preset.value, theme_presets, 5 ) )
		{
			this->apply_theme_preset( theme.preset.value );
			detail::status_msg = "theme preset applied";
			detail::status_timer = 2.0f;
		}
		xui::tooltip( "Select a complete menu color preset." );
		xui::layout::separator( );

		// Name field drives create/overwrite; search filters the list
		xui::text_input( "##cfg_name", detail::name_buf, 64, "config name..." );
		xui::tooltip( "Name used when saving or overwriting a config." );

		xui::text_input( "##cfg_search", detail::search_buf, 64, "filter list..." );
		xui::tooltip( "Filter the list below by name. Does not change the save name." );

		constexpr auto btn_h{ 28.0f };
		constexpr auto status_h{ 18.0f };
		const auto [ avail_w, avail_h ] = xui::layout::avail( );
		const auto list_h = std::max( 100.0f, avail_h - btn_h - status_h - s.item_spacing_y * 3.0f );

		if ( xui::begin_child( "##cfg_list", avail_w, list_h, true ) )
		{
			const auto row_w = xui::layout::avail( ).first;
			constexpr auto row_h{ 28.0f };
			auto visible_rows{ 0 };

			for ( auto i = 0; i < static_cast< int >( detail::config_list.size( ) ); ++i )
			{
				const auto& wname = detail::config_list[ i ];

				if ( !detail::config_matches_search( wname ) )
				{
					continue;
				}

				char narrow[ 128 ]{};
				detail::wide_to_utf8( wname, narrow, sizeof( narrow ) );

				const auto row = xui::layout::item( row_w, row_h );
				const auto is_selected = ( detail::selected == i );
				const auto is_hovered = input.in_rect( row );

				if ( is_hovered && input.mouse_clicked && !xui::ctx( ).overlay_blocking( ) )
				{
					detail::selected = i;
					detail::name_buf = narrow;
					detail::confirm_delete = false;
					detail::confirm_reset = false;
					if ( config::registry::load( wname ) )
					{
						settings::finalize_binds( );
						detail::status_msg = std::string( "loaded " ) + narrow;
						detail::status_timer = 2.5f;
					}
					else
					{
						detail::status_msg = "load failed";
						detail::status_timer = 2.5f;
					}
				}

				const auto hover_anim = xui::anim::lerp( xui::fnv1a( "cfgrow" ) + i, is_hovered ? 1.0f : 0.0f, 14.0f );
				const auto sel_anim = xui::anim::lerp( xui::fnv1a( "cfgsel" ) + i, is_selected ? 1.0f : 0.0f, 10.0f );

				if ( sel_anim > 0.01f )
				{
					dl.rect_filled( row.x, row.y, row.w, row.h, tokens::col_accent.alpha( static_cast< std::uint8_t >( 70.0f * sel_anim ) ), xdraw::corner_radius{ 6.0f } );
				}
				else if ( hover_anim > 0.01f )
				{
					dl.rect_filled( row.x, row.y, row.w, row.h, tokens::col_elevated.alpha( static_cast< std::uint8_t >( 255.0f * hover_anim * 0.5f ) ), xdraw::corner_radius{ 6.0f } );
				}

				const auto [ tw, th ] = xdraw::measure_text( narrow );
				( void )tw;
				const auto text_col = is_selected
					? xui::lerp( tokens::col_text, tokens::col_accent, sel_anim )
					: xui::lerp( tokens::col_text_dim, tokens::col_text, hover_anim );

				dl.text( row.x + 10.0f, row.y + ( row.h - th ) * 0.5f, narrow, text_col );
				visible_rows++;
			}

			if ( visible_rows == 0 )
			{
				const auto row = xui::layout::item( row_w, row_h );
				dl.text( row.x + 10.0f, row.y + 6.0f, detail::config_list.empty( ) ? "no configs found — type a name and hit save" : "no matches", tokens::col_text_dim );
			}

			xui::end_child( );
		}

		const auto btn_w = std::max( 48.0f, ( avail_w - s.item_spacing_x * 4.0f ) / 5.0f );
		const auto has_selection = detail::selected >= 0 && detail::selected < static_cast< int >( detail::config_list.size( ) );

		auto trim = [ ]( std::string name )
		{
			while ( !name.empty( ) && ( name.front( ) == ' ' || name.front( ) == '\t' ) )
				name.erase( name.begin( ) );
			while ( !name.empty( ) && ( name.back( ) == ' ' || name.back( ) == '\t' ) )
				name.pop_back( );
			return name;
		};

		const auto save_name = trim( !detail::name_buf.empty( ) ? detail::name_buf : ( has_selection ? detail::selected_name( ) : detail::search_buf ) );
		const auto can_save = !save_name.empty( );

		if ( xui::button( "save", btn_w, btn_h ) )
		{
			if ( !can_save )
			{
				detail::status_msg = "enter a name first";
				detail::status_timer = 2.5f;
			}
			else
			{
				const auto wname = detail::utf8_to_wide( save_name );
				if ( config::registry::save( wname ) )
				{
					detail::name_buf = save_name;
					detail::needs_refresh = true;
					detail::selected = -1;
					detail::status_msg = std::string( "saved " ) + save_name;
					detail::status_timer = 2.5f;
				}
				else
				{
					detail::status_msg = "save failed";
					detail::status_timer = 2.5f;
				}
			}
		}
		xui::tooltip( "Write current settings using the name field.\nCreates a new file if that name does not exist." );

		xui::layout::same_line( );

		if ( detail::confirm_reset )
		{
			if ( xui::button( "confirm##reset", btn_w, btn_h ) )
			{
				detail::reset_defaults( );
				detail::confirm_reset = false;
				detail::status_msg = "reset to defaults";
				detail::status_timer = 2.5f;
			}
		}
		else if ( xui::button( "reset", btn_w, btn_h ) )
		{
			detail::confirm_reset = true;
			detail::confirm_delete = false;
			detail::confirm_timer = 0.0f;
		}
		xui::tooltip( "Restore every setting to factory defaults.\nClick again within 3 seconds to confirm." );

		xui::layout::same_line( );

		if ( detail::confirm_delete )
		{
			if ( xui::button( "confirm##delete", btn_w, btn_h ) && has_selection )
			{
				config::registry::remove( detail::config_list[ detail::selected ] );
				detail::selected = -1;
				detail::needs_refresh = true;
				detail::confirm_delete = false;
				detail::status_msg = "deleted";
				detail::status_timer = 2.5f;
			}
		}
		else if ( xui::button( "delete", btn_w, btn_h ) )
		{
			if ( !has_selection )
			{
				detail::status_msg = "select a config first";
				detail::status_timer = 2.5f;
			}
			else
			{
				detail::confirm_delete = true;
				detail::confirm_reset = false;
				detail::confirm_timer = 0.0f;
			}
		}
		xui::tooltip( "Permanently delete the selected config file.\nClick again within 3 seconds to confirm." );

		xui::layout::same_line( );

		if ( xui::button( "import", btn_w, btn_h ) )
		{
			const auto clip = detail::paste_from_clipboard( );
			if ( clip.empty( ) )
			{
				detail::status_msg = "clipboard empty";
				detail::status_timer = 2.5f;
			}
			else
			{
				const auto result = config::import_auto( clip );
				if ( result.success )
				{
					settings::finalize_binds( );

					std::wstring wname{};
					if ( !result.name.empty( ) )
					{
						wname = detail::utf8_to_wide( result.name );
					}
					else if ( !detail::name_buf.empty( ) )
					{
						wname = detail::utf8_to_wide( trim( detail::name_buf ) );
					}
					else
					{
						SYSTEMTIME st{};
						GetLocalTime( &st );
						wchar_t buf[ 128 ]{};
						std::swprintf( buf, 128, L"import_%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond );
						wname = buf;
					}

					if ( config::registry::save( wname ) )
					{
						char narrow[ 128 ]{};
						detail::wide_to_utf8( wname, narrow, sizeof( narrow ) );
						detail::name_buf = narrow;
						detail::needs_refresh = true;
						detail::status_msg = std::string( "imported " ) + narrow;
						detail::status_timer = 2.5f;
					}
					else
					{
						detail::status_msg = "import applied, save failed";
						detail::status_timer = 2.5f;
					}
				}
				else
				{
					detail::status_msg = "import failed";
					detail::status_timer = 2.5f;
				}
			}
		}
		xui::tooltip( "Paste a shared config string from the clipboard and apply it." );

		xui::layout::same_line( );

		if ( xui::button( "export", btn_w, btn_h ) )
		{
			const auto name = !save_name.empty( ) ? save_name : ( has_selection ? detail::selected_name( ) : std::string{} );
			const auto code = config::export_share_words( name );
			if ( !code.empty( ) && detail::copy_to_clipboard( code ) )
			{
				detail::status_msg = "exported to clipboard";
				detail::status_timer = 2.5f;
			}
			else
			{
				detail::status_msg = "export failed";
				detail::status_timer = 2.5f;
			}
		}
		xui::tooltip( "Copy the current config as a shareable string to the clipboard." );

		xui::layout::separator( );

		xui::color_picker( "accent color", theme.accent );
		xui::tooltip( "Set the highlight color used by active controls and accents." );
		xui::color_picker( "background color", theme.background );
		xui::tooltip( "Set the main menu window background color." );
		xui::color_picker( "panel color", theme.panel );
		xui::tooltip( "Set the color used by cards, child panels, and control surfaces." );
		xui::color_picker( "text color", theme.text );
		xui::tooltip( "Set the primary menu text color." );
		xui::color_picker( "muted text color", theme.text_dim );
		xui::tooltip( "Set the secondary text color used for hints and inactive labels." );
		xui::color_picker( "border color", theme.border );
		xui::tooltip( "Set the outline color used around panels and controls." );
		xui::slider_float( "menu opacity", theme.opacity, 15.0f, 100.0f, "%.0f%%" );
		xui::tooltip( "Control the transparency of the menu background, panels, popups, and borders." );
		if ( xui::button( "reset theme", 120.0f, 24.0f ) )
		{
			this->apply_theme_preset( 0 );
			detail::status_msg = "theme reset";
			detail::status_timer = 2.0f;
		}
		xui::tooltip( "Restore the default blue menu theme." );

		xui::layout::separator( );

		if ( xui::button( "create diagnostic package", 220.0f, btn_h ) )
		{
			if ( detail::create_diagnostic_package( ) )
			{
				char path[ MAX_PATH * 3 ]{};
				WideCharToMultiByte( CP_UTF8, 0, detail::last_diagnostic_path.c_str( ), -1, path, sizeof( path ), nullptr, nullptr );
				detail::copy_to_clipboard( path );
				detail::status_msg = "diagnostic package created; path copied";
				rendering::g_widgets.notify( "diagnostics ready", "package path copied to clipboard" );
			}
			else
			{
				detail::status_msg = "diagnostic package failed";
				rendering::g_widgets.notify( "diagnostic error", "package creation failed" );
			}
			detail::status_timer = 4.0f;
		}
		xui::tooltip( "Create a crash snapshot, logs, previous crash dump, and system report.\nThe package folder path is copied to the clipboard." );

		const auto build_row = xui::layout::item( avail_w, 22.0f );
		const std::string build_line = std::string( "build: development x64 | " ) + __DATE__ + " " + __TIME__;
		dl.text( build_row.x + 4.0f, build_row.y + 3.0f, build_line.c_str( ), tokens::col_text );

		static constexpr const char* updates[ ]
		{
			"latest: spectator viewing indicators and avatar controls",
			"latest: damage numbers with classic, circle, and cross styles",
			"latest: one-click diagnostic package and manual snapshot",
			"previous: 241 validated custom agents and first-person arm isolation",
			"previous: prediction state hardening for mid-match stability"
		};
		for ( const auto* update : updates )
		{
			const auto row = xui::layout::item( avail_w, 19.0f );
			dl.text( row.x + 4.0f, row.y + 2.0f, update, tokens::col_text_dim );
		}

		if ( !detail::status_msg.empty( ) )
		{
			const auto row = xui::layout::item( avail_w, status_h );
			dl.text( row.x + 4.0f, row.y + 2.0f, detail::status_msg.c_str( ), tokens::col_accent );
		}

		xui::end_child( );
	}


} // namespace rendering
