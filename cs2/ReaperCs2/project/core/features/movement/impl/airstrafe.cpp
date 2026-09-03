#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/logging/logging.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>
#include <protection/game_addresses.hpp>

namespace features::movement {

	void airstrafe::on_create_move( systems::input::usercmd* cmd )
	{
		if ( !cmd || !settings::g_movement.auto_strafe.value )
		{
			return;
		}

		if ( settings::g_movement.m_test_strafer.enabled.value )
		{
			return;
		}

		if ( features::movement::g_jumpbug.active_this_tick( ) )
		{
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.is_alive || !local.pawn )
		{
			return;
		}

		const auto move_type = memory::read<int>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
		if ( move_type == cstypes::move_type::ladder || move_type == cstypes::move_type::noclip )
		{
			return;
		}

		const auto& prestate = systems::g_prediction.pre( );
		if ( prestate.flags & cstypes::entity_flags::on_ground )
		{
			return;
		}

		if ( cmd->buttons.value & cstypes::command_buttons::in_sprint )
		{
			return;
		}

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( !base )
		{
			return;
		}

		const auto vel = prestate.networked_velocity.length_2d( ) > 1.0f ? prestate.networked_velocity : prestate.velocity;
		const auto speed = vel.length_2d( );
		if ( speed < 5.0f )
		{
			return;
		}

		const auto normalize = []( float value )
		{
			while ( value > 180.0f ) value -= 360.0f;
			while ( value < -180.0f ) value += 360.0f;
			return value;
		};

		const auto current_buttons = cmd->buttons.value;
		this->check_button( current_buttons, cstypes::command_buttons::in_moveleft );
		this->check_button( current_buttons, cstypes::command_buttons::in_moveright );
		this->check_button( current_buttons, cstypes::command_buttons::in_forward );
		this->check_button( current_buttons, cstypes::command_buttons::in_back );
		this->m_last_buttons = current_buttons;

		const auto cam_yaw = systems::g_input.get_view_angles( ).y;
		auto yaw = cam_yaw;

		if ( settings::g_movement.auto_strafe_type.value == 0 )
		{
			const auto pressing_left = ( current_buttons & cstypes::command_buttons::in_moveleft ) != 0;
			const auto pressing_right = ( current_buttons & cstypes::command_buttons::in_moveright ) != 0;
			const auto pressing_forward = ( current_buttons & cstypes::command_buttons::in_forward ) != 0;
			const auto pressing_back = ( current_buttons & cstypes::command_buttons::in_back ) != 0;

			if ( pressing_left )
			{
				this->m_side_switch = false;
			}
			if ( pressing_right )
			{
				this->m_side_switch = true;
			}

			if ( !pressing_left && !pressing_right )
			{
				return;
			}

			auto wish_yaw = this->m_side_switch ? 90.0f : -90.0f;
			if ( pressing_forward )
			{
				wish_yaw *= 0.5f;
			}
			else if ( pressing_back )
			{
				wish_yaw = -wish_yaw * 0.5f + 180.0f;
			}

			auto velocity_dir = 180.0f / 3.14159265358979323846f * std::atan2( vel.y, vel.x );
			velocity_dir = normalize( velocity_dir );

			auto optimal_angle = settings::g_movement.directional_type.value == 0
				? std::clamp( 180.0f / 3.14159265358979323846f * std::atan( 15.0f / speed ), 0.0f, 45.0f )
				: std::clamp( 180.0f / 3.14159265358979323846f * std::atan( 30.0f / speed ), 0.0f, 90.0f );

			const auto smooth_factor = settings::g_movement.strafe_smooth.value / 100.0f;
			optimal_angle *= ( 1.0f - smooth_factor * 0.5f );

			auto target_yaw = velocity_dir + ( wish_yaw > 0.0f ? optimal_angle : -optimal_angle );
			target_yaw = normalize( target_yaw );

			if ( settings::g_movement.silent_strafe.value )
			{
				const auto angle_diff = normalize( target_yaw - math::helpers::rad_to_deg( std::atan2( vel.y, vel.x ) ) );
				const auto rot = angle_diff * ( 3.14159265358979323846f / 180.0f );
				base->set_forwardmove( std::clamp( std::cos( rot ), -1.0f, 1.0f ) );
				base->set_leftmove( std::clamp( -std::sin( rot ), -1.0f, 1.0f ) );
			}
			else
			{
				base->mutable_viewangles( )->set_y( target_yaw );
				base->set_forwardmove( 0.0f );
				base->set_leftmove( wish_yaw > 0.0f ? -1.0f : 1.0f );
			}

			return;
		}

		auto offset = 0.0f;
		if ( this->m_last_pressed & cstypes::command_buttons::in_moveleft )
		{
			offset += 90.0f;
		}
		if ( this->m_last_pressed & cstypes::command_buttons::in_moveright )
		{
			offset -= 90.0f;
		}
		if ( this->m_last_pressed & cstypes::command_buttons::in_forward )
		{
			offset *= 0.5f;
		}
		else if ( this->m_last_pressed & cstypes::command_buttons::in_back )
		{
			offset = -offset * 0.5f + 180.0f;
		}

		yaw = normalize( yaw + offset );

		auto velocity_angle = 180.0f / 3.14159265358979323846f * std::atan2f( vel.y, vel.x );
		velocity_angle = normalize( velocity_angle );

		const auto ideal = speed > 0.0f
			? std::clamp( 180.0f / 3.14159265358979323846f * std::atan2( 40.0f / speed, 1.0f ), 0.0f, 90.0f )
			: 90.0f;
		const auto correct = 0.0f;

		base->set_forwardmove( 0.0f );
		base->set_leftmove( 0.0f );

		auto velocity_delta = normalize( yaw - velocity_angle );
		if ( std::fabs( velocity_delta ) > 90.0f )
		{
			velocity_delta = std::copysign( 90.0f, velocity_delta );
		}

		if ( speed <= 80.0f )
		{
			yaw += ideal * 3.0f;
			base->set_leftmove( 1.0f );
		}
		else if ( velocity_delta > correct )
		{
			yaw = velocity_angle + correct * 4.0f;
			base->set_leftmove( -1.0f );
		}
		else if ( velocity_delta < -correct )
		{
			yaw = velocity_angle - correct * 4.0f;
			base->set_leftmove( 1.0f );
		}
		else
		{
			yaw += ideal * 4.0f;
			base->set_leftmove( 1.0f );
		}

		this->rotate_movement( base, normalize( yaw ), cam_yaw );
		return;
	}
	void airstrafe::store_angles( )
	{
		this->m_angles = systems::g_input.get_view_angles( );
	}

	void airstrafe::check_button( std::uintptr_t current_buttons, std::uintptr_t button )
	{
		constexpr auto moveleft = static_cast< std::uintptr_t >( cstypes::command_buttons::in_moveleft );
		constexpr auto moveright = static_cast< std::uintptr_t >( cstypes::command_buttons::in_moveright );
		constexpr auto forward = static_cast< std::uintptr_t >( cstypes::command_buttons::in_forward );
		constexpr auto back = static_cast< std::uintptr_t >( cstypes::command_buttons::in_back );

		if ( current_buttons & button && ( !( this->m_last_buttons & button ) || ( button & moveleft && !( this->m_last_pressed & moveright ) ) || ( button & moveright && !( this->m_last_pressed & moveleft ) ) || ( button & forward && !( this->m_last_pressed & back ) ) || ( button & back && !( this->m_last_pressed & forward ) ) ) )
		{
			if ( button & moveleft )
			{
				this->m_last_pressed &= ~moveright;
			}
			else if ( button & moveright )
			{
				this->m_last_pressed &= ~moveleft;
			}
			else if ( button & forward )
			{
				this->m_last_pressed &= ~back;
			}
			else if ( button & back )
			{
				this->m_last_pressed &= ~forward;
			}

			this->m_last_pressed |= button;
		}
		else if ( !( current_buttons & button ) )
		{
			this->m_last_pressed &= ~button;
		}
	}

	void airstrafe::rotate_movement( proto::base_usercmd_pb* base, float target_yaw, float view_yaw ) const
	{
		const auto forward_move = base->forwardmove( );
		const auto side_move = base->leftmove( );

		math::vector3 target_forward{}, target_right{};
		math::helpers::angle_vectors_2d( target_yaw, target_forward, target_right );

		math::vector3 view_forward{}, view_right{};
		math::helpers::angle_vectors_2d( view_yaw, view_forward, view_right );

		const auto tf = target_forward * forward_move;
		const auto tr = target_right * side_move;

		const auto corrected_forward = view_forward.dot( tf ) + view_forward.dot( tr );
		const auto corrected_side = view_right.dot( tf ) + view_right.dot( tr );

		base->set_forwardmove( std::clamp( -corrected_forward, -1.0f, 1.0f ) );
		base->set_leftmove( std::clamp( -corrected_side, -1.0f, 1.0f ) );
	}

	void airstrafe::rotate_to_stop( proto::base_usercmd_pb* base, const math::vector3& velocity ) const
	{
		const auto speed = velocity.length_2d( );
		const auto wish_yaw = std::atan2f( velocity.y, velocity.x ) * ( 180.0f / std::numbers::pi_v<float> ) + 180.0f;

		{
			const auto& ctx = features::combat::g_shared.ctx( );
			const auto max_speed = ( ctx.valid && ctx.weapon_vdata ) ? memory::read<float>( ctx.weapon_vdata + SCHEMA( "CCSWeaponBaseVData", "m_flMaxSpeed"_hash ) ) : 250.0f;

			base->set_forwardmove( std::clamp( speed / max_speed, 0.0f, 1.0f ) );
			base->set_leftmove( 0.0f );
		}

		const auto rotation = ( base->viewangles( )->y( ) - wish_yaw ) * ( std::numbers::pi_v<float> / 180.0f );
		const auto fwd = base->forwardmove( );
		const auto side = base->leftmove( );

		base->set_forwardmove( std::clamp( std::cosf( rotation ) * fwd - std::sinf( rotation ) * side, -1.0f, 1.0f ) );
		base->set_leftmove( std::clamp( ( std::sinf( rotation ) * fwd + std::cosf( rotation ) * side ) * -1.0f, -1.0f, 1.0f ) );
	}

} // namespace features::movement
