#include <pch/pch.hpp>
#include <core/resources/fonts/inter.hpp>
#include <core/resources/fonts/pixel7.hpp>
#include "../rendering.hpp"

#include <fstream>
#include <vector>

namespace rendering {

	namespace {

		std::vector<std::byte> g_ui_regular{};
		std::vector<std::byte> g_ui_bold{};

		[[nodiscard]] bool read_windows_font( const wchar_t* file_name, std::vector<std::byte>& out )
		{
			wchar_t windir[ MAX_PATH ]{};
			if ( !GetWindowsDirectoryW( windir, MAX_PATH ) )
			{
				return false;
			}

			std::wstring path = windir;
			path += L"\\Fonts\\";
			path += file_name;

			std::ifstream file( path, std::ios::binary | std::ios::ate );
			if ( !file )
			{
				return false;
			}

			const auto size = static_cast< std::size_t >( file.tellg( ) );
			if ( size < 1024 )
			{
				return false;
			}

			file.seekg( 0, std::ios::beg );
			out.resize( size );
			file.read( reinterpret_cast< char* >( out.data( ) ), static_cast< std::streamsize >( size ) );
			return static_cast< std::size_t >( file.gcount( ) ) == size;
		}

	} // namespace

	bool fonts::load_windows_ui_fonts( )
	{
		// Prefer Semibold/Bold so menu type is thicker
		if ( !read_windows_font( L"segoeuisb.ttf", g_ui_regular ) &&
			 !read_windows_font( L"segoeuib.ttf", g_ui_regular ) &&
			 !read_windows_font( L"segoeui.ttf", g_ui_regular ) )
		{
			return false;
		}

		if ( !read_windows_font( L"segoeuib.ttf", g_ui_bold ) )
		{
			g_ui_bold = g_ui_regular;
		}

		this->load_family( this->inter_medium, std::span<const std::byte>{ g_ui_regular.data( ), g_ui_regular.size( ) }, { 13.0f, 15.5f, 18.0f } );
		this->load_family( this->inter_bold, std::span<const std::byte>{ g_ui_bold.data( ), g_ui_bold.size( ) }, { 13.0f, 15.5f, 18.0f } );
		return this->inter_medium[ size::normal ] != nullptr;
	}

	void fonts::initialize( )
	{
		if ( !this->load_windows_ui_fonts( ) )
		{
			this->load_family( this->inter_medium, std::as_bytes( std::span{ resources::fonts::inter::regular } ), { 12.0f, 15.0f, 18.0f } );
			this->load_family( this->inter_bold, std::as_bytes( std::span{ resources::fonts::inter::bold } ), { 12.0f, 15.0f, 18.0f } );
		}

		this->load_family( this->smallest_pixel7, std::as_bytes( std::span{ resources::fonts::pixel7::smallest } ), { 9.0f, 10.5f, 14.0f } );
	}

	void fonts::push_menu( )
	{
		auto* f = this->inter_bold[ size::normal ];
		if ( !f )
		{
			f = this->inter_medium[ size::normal ];
		}
		if ( f )
		{
			xdraw::push_font( f );
		}
	}

	void fonts::pop_menu( )
	{
		if ( this->inter_medium[ size::normal ] )
		{
			xdraw::pop_font( );
		}
	}

	void fonts::load_family( family_t& family, std::span<const std::byte> data, const std::array<float, static_cast< std::size_t >( size::count )>& sizes )
	{
		for ( auto i = 0ull; i < sizes.size( ); ++i )
		{
			family.sizes[ i ] = xdraw::load_font( data, sizes[ i ] );
		}
	}

} // namespace rendering
