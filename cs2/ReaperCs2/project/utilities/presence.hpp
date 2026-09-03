#pragma once

#include <external/nlohmann/json.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/steam/steam.hpp>
#include <utilities/diag.hpp>
#include <core/settings.hpp>
#include <core/systems/systems.hpp>

namespace presence {

	struct peer
	{
		std::uint64_t steam_id{};
		std::int16_t ct_agent{};
		std::int16_t t_agent{};
		std::vector<std::pair<std::int16_t, int>> skins{};
	};

	namespace detail {
		struct handoff
		{
			char magic[ 8 ];
			DWORD version;
			DWORD owner_pid;
			char token[ 64 ];
		};

		inline std::string token{};
		inline std::atomic_bool running{};
		inline std::thread worker{};
		inline std::shared_mutex peers_mutex{};
		inline std::unordered_map<std::uint64_t, peer> peers{};
		inline std::atomic_int last_http_status{};
		inline std::atomic_uint64_t successful_heartbeats{};
		inline std::atomic_uint64_t failed_heartbeats{};
		inline std::atomic_size_t last_roster_size{};
		inline std::atomic_size_t last_peer_count{};

		inline void log_status( const char* phase, int http_status, std::size_t roster_size, std::size_t peer_count )
		{
			char message[ 256 ]{};
			_snprintf_s( message, sizeof( message ), _TRUNCATE,
				"presence: %s http=%d roster=%zu peers=%zu ok=%llu failed=%llu",
				phase, http_status, roster_size, peer_count,
				static_cast<unsigned long long>( successful_heartbeats.load( ) ),
				static_cast<unsigned long long>( failed_heartbeats.load( ) ) );
			diag::write( http_status >= 200 && http_status < 300 ? diag::level::info : diag::level::warning, message );
		}

		inline std::vector<std::uint64_t> roster( )
		{
			std::vector<std::uint64_t> result{};
			result.reserve( 64 );
			if ( const auto local_id = steam::user::get_steam_id( ) )
				result.push_back( local_id );

			for ( const auto& player : systems::g_entities.get_by_type( systems::entities::type::player ) )
			{
				const auto id = memory::safe_read<std::uint64_t>( player.ptr + SCHEMA( "CBasePlayerController", "m_steamID"_hash ) ).value_or( 0 );
				if ( id >= 76561190000000000ull )
					result.push_back( id );
			}
			std::ranges::sort( result );
			result.erase( std::unique( result.begin( ), result.end( ) ), result.end( ) );
			if ( result.size( ) > 64 ) result.resize( 64 );
			return result;
		}

		inline void heartbeat( )
		{
			const auto local_id = steam::user::get_steam_id( );
			if ( !local_id || token.empty( ) )
			{
				++failed_heartbeats;
				log_status( token.empty( ) ? "token missing" : "steam id missing", 0, 0, 0 );
				return;
			}

			nlohmann::json body{
				{ "steam_id", std::to_string( local_id ) },
				{ "ct_agent", settings::g_changer.agents.ct_def },
				{ "t_agent", settings::g_changer.agents.t_def },
				{ "roster", nlohmann::json::array( ) },
				{ "skins", nlohmann::json::array( ) }
			};
			const auto current_roster = roster( );
			last_roster_size.store( current_roster.size( ) );
			for ( const auto id : current_roster ) body[ "roster" ].push_back( std::to_string( id ) );
			for ( const auto& [ def, skin ] : settings::g_changer.skins.data )
				body[ "skins" ].push_back( { { "def", def }, { "paint", skin.paint_kit_id } } );

			const auto data = body.dump( );
			const auto req = steam::http::create_post(
				"https://kryptik-production.up.railway.app/presence/heartbeat",
				"application/json", data.data( ), static_cast<std::uint32_t>( data.size( ) ) );
			if ( !req )
			{
				++failed_heartbeats;
				log_status( "request creation failed", 0, current_roster.size( ), 0 );
				return;
			}
			const auto auth = std::string( "Bearer " ) + token;
			steam::http::set_header( req, "Authorization", auth.c_str( ) );
			steam::http::set_timeout( req, 8 );

			std::unordered_map<std::uint64_t, peer> next{};
			int http_status{};
			bool parsed{};
			if ( steam::http::send_and_wait( req, 9000, &http_status ) )
			{
				std::vector<std::uint8_t> response{};
				if ( steam::http::get_response_body( req, response ) )
				{
					try
					{
						const auto json = nlohmann::json::parse( response.begin( ), response.end( ) );
						for ( const auto& item : json.value( "users", nlohmann::json::array( ) ) )
						{
							peer value{};
							value.steam_id = std::stoull( item.value( "steam_id", "0" ) );
							value.ct_agent = static_cast<std::int16_t>( item.value( "ct_agent", 0 ) );
							value.t_agent = static_cast<std::int16_t>( item.value( "t_agent", 0 ) );
							for ( const auto& skin : item.value( "skins", nlohmann::json::array( ) ) )
								value.skins.emplace_back( static_cast<std::int16_t>( skin.value( "def", 0 ) ), skin.value( "paint", 0 ) );
							if ( value.steam_id ) next.emplace( value.steam_id, std::move( value ) );
						}
						parsed = true;
					}
					catch ( ... ) {}
				}
			}
			steam::http::release( req );
			last_http_status.store( http_status );
			last_peer_count.store( next.size( ) );
			if ( parsed ) ++successful_heartbeats; else ++failed_heartbeats;
			log_status( parsed ? "heartbeat ok" : "heartbeat failed", http_status, current_roster.size( ), next.size( ) );
			std::unique_lock lock( peers_mutex );
			peers = std::move( next );
		}

		inline void run( )
		{
			while ( running.load( std::memory_order_acquire ) )
			{
				heartbeat( );
				for ( int i = 0; i < 100 && running.load( std::memory_order_acquire ); ++i )
					std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
			}
		}
	}

	inline bool capture_handoff( )
	{
		wchar_t name[ 96 ]{};
		swprintf_s( name, L"Local\\KryptiKPresence_%lu", GetCurrentProcessId( ) );
		const auto mapping = OpenFileMappingW( FILE_MAP_ALL_ACCESS, FALSE, name );
		if ( !mapping )
		{
			diag::write( diag::level::warning, "presence: loader handoff mapping missing" );
			return false;
		}
		auto* view = static_cast<detail::handoff*>( MapViewOfFile( mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof( detail::handoff ) ) );
		if ( view && std::memcmp( view->magic, "KRYPTIK", 7 ) == 0 && view->version == 1 && view->owner_pid == GetCurrentProcessId( ) )
		{
			const auto length = strnlen_s( view->token, sizeof( view->token ) );
			if ( length > 0 && length < sizeof( view->token ) ) detail::token.assign( view->token, length );
			SecureZeroMemory( view, sizeof( *view ) );
			FlushViewOfFile( view, sizeof( *view ) );
		}
		if ( view ) UnmapViewOfFile( view );
		CloseHandle( mapping );
		const auto captured = !detail::token.empty( );
		diag::write( captured ? diag::level::info : diag::level::warning,
			captured ? "presence: loader handoff captured" : "presence: loader handoff invalid or empty" );
		return captured;
	}

	inline void start( )
	{
		if ( detail::token.empty( ) || detail::running.exchange( true ) ) return;
		detail::worker = std::thread( detail::run );
	}

	inline void shutdown( )
	{
		detail::running.store( false, std::memory_order_release );
		if ( detail::worker.joinable( ) ) detail::worker.join( );
		std::unique_lock lock( detail::peers_mutex );
		detail::peers.clear( );
		if ( !detail::token.empty( ) ) SecureZeroMemory( detail::token.data( ), detail::token.size( ) );
		detail::token.clear( );
	}

	inline std::optional<peer> find( std::uint64_t steam_id )
	{
		if ( !detail::token.empty( ) && steam_id != 0 && steam_id == steam::user::get_steam_id( ) )
		{
			peer local{};
			local.steam_id = steam_id;
			local.ct_agent = settings::g_changer.agents.ct_def;
			local.t_agent = settings::g_changer.agents.t_def;
			local.skins.reserve( settings::g_changer.skins.data.size( ) );
			for ( const auto& [ def, skin ] : settings::g_changer.skins.data )
				local.skins.emplace_back( def, skin.paint_kit_id );
			return local;
		}

		std::shared_lock lock( detail::peers_mutex );
		const auto it = detail::peers.find( steam_id );
		if ( it == detail::peers.end( ) ) return std::nullopt;
		return it->second;
	}

	inline bool available( )
	{
		return !detail::token.empty( );
	}

	inline bool is_local_user( std::uint64_t steam_id )
	{
		return available( ) && steam_id != 0 && steam_id == steam::user::get_steam_id( );
	}
}
