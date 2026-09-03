#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>

#include "../movement.hpp"
#include <protection/game_addresses.hpp>

namespace features::movement {

	namespace {

		[[nodiscard]] std::optional<float> predict_landing_fraction(
			std::uintptr_t local_pawn,
			std::uintptr_t movement_services,
			const systems::prediction::state& prestate,
			bool holding_duck )
		{
			if ( prestate.networked_velocity.z > 0.0f )
			{
				return std::nullopt;
			}

			const auto duck_amount = memory::read<float>( movement_services + SCHEMA( "CCSPlayer_MovementServices", "m_flDuckAmount"_hash ) );
			const auto mins = memory::read<math::vector3>( local_pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash ) + SCHEMA( "CCollisionProperty", "m_vecMins"_hash ) );
			auto maxs = memory::read<math::vector3>( local_pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash ) + SCHEMA( "CCollisionProperty", "m_vecMaxs"_hash ) );

			auto trace_origin = prestate.networked_origin;
			if ( holding_duck && duck_amount > 0.0f )
			{
				const auto standing_height{ 72.0f };
				const auto duck_hull_diff = standing_height - maxs.z;
				trace_origin.z -= duck_hull_diff * 0.5f;
				maxs.z = standing_height;
			}

			auto trace_mask{ 0ull };
			{
				const auto pawn_ptr = memory::read<std::uintptr_t>( movement_services + 56 );
				trace_mask = memory::read<std::uintptr_t>( pawn_ptr + 0xd48 );

				if ( !pawn_ptr || ( memory::read<std::uint32_t>( pawn_ptr + 0x3f8 ) & 0x10 ) )
				{
					trace_mask |= 0x20;
				}
			}

			const auto filter = systems::g_tracing.make_player_movement_filter( local_pawn, trace_mask, 11 );
			const auto sv_gravity = CONVAR ("sv_gravity")->get<float>( );
			const auto sv_standable_normal = CONVAR ("sv_standable_normal")->get<float>( );
			const auto gravity_scale = memory::read<float>( local_pawn + SCHEMA( "C_BaseEntity", "m_flGravityScale"_hash ) );

			auto velocity = prestate.networked_velocity;
			velocity.z -= ( gravity_scale * sv_gravity * cstypes::tick_interval ) * 0.5f;

			const math::vector3 trace_start = trace_origin;
			math::vector3 trace_end{};

			trace_end.x = trace_origin.x + velocity.x * cstypes::tick_interval;
			trace_end.y = trace_origin.y + velocity.y * cstypes::tick_interval;
			trace_end.z = trace_origin.z + velocity.z * cstypes::tick_interval;
			trace_end.z -= 2.0f;

			const auto result = systems::g_tracing.trace_player_bbox( trace_start, trace_end, { mins, maxs }, filter, movement_services );
			if ( result.fraction >= 1.0f || result.normal.z < sv_standable_normal )
			{
				return std::nullopt;
			}

			// fraction 0 = already touching this tick — jump immediately
			if ( result.fraction <= 0.0f )
			{
				return 1.0f / 64.0f;
			}

			auto frac = result.fraction;
			return std::clamp( std::round( frac * 64.0f ) / 64.0f, 1.0f / 64.0f, 63.0f / 64.0f );
		}

		void apply_landing_jump( proto::base_usercmd_pb* base, float when )
		{
			if ( !base )
			{
				return;
			}

			const auto step = systems::g_input.acquire_subtick_step( base->mutable_subtick_moves( ) );
			if ( !step )
			{
				return;
			}

			step->set_button( cstypes::command_buttons::in_jump );
			step->set_pressed( true );
			step->set_when( std::clamp( when, 1.0f / 64.0f, 63.0f / 64.0f ) );
		}

	} // namespace

	void bhop::on_create_move( systems::input::usercmd* cmd ) const
	{
		if ( !settings::g_movement.bhop.value || !cmd )
		{
			return;
		}

		if ( auto* cvar = CONVAR( "sv_autobunnyhopping" ) )
		{
			if ( cvar->get<bool>( ) )
			{
				return;
			}
		}

		const auto jump = static_cast< std::uintptr_t >( cstypes::command_buttons::in_jump );

		auto holding = ( cmd->buttons.value & jump ) != 0
			|| ( cmd->buttons.value_scroll & jump ) != 0;

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( base )
		{
			if ( const auto* pb = base->buttons_pb( ) )
			{
				holding = holding || ( pb->buttonstate1( ) & jump ) != 0;
			}
		}

		if ( !holding )
		{
			return;
		}

		if ( features::movement::g_jumpbug.active_this_tick( ) )
		{
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.pawn )
		{
			return;
		}

		const auto move_type = memory::read<std::uint8_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
		if ( move_type == cstypes::move_type::ladder || move_type == cstypes::move_type::noclip )
		{
			return;
		}

		const auto& prestate = systems::g_prediction.pre( );
		const auto grounded = ( prestate.flags & cstypes::entity_flags::on_ground ) != 0;

		if ( grounded )
		{
			cmd->buttons.value |= jump;
			return;
		}

		// Airborne: release so the next ground tick can press again.
		cmd->buttons.value &= ~jump;
		if ( base )
		{
			if ( auto* pb = const_cast< proto::in_button_state_pb* >( base->buttons_pb( ) ) )
			{
				pb->set_buttonstate1( pb->buttonstate1( ) & ~jump );
			}
		}

		if ( !base )
		{
			return;
		}

		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !movement_services )
		{
			return;
		}

		const auto holding_duck = ( cmd->buttons.value & cstypes::command_buttons::in_duck ) != 0;
		if ( const auto landing = predict_landing_fraction( local.pawn, movement_services, prestate, holding_duck ) )
		{
			apply_landing_jump( base, *landing );
		}
	}

} // namespace features::movement
