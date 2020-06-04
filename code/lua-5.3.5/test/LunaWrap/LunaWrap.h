#ifndef _LUNA_WRAP_H_
#define _LUNA_WRAP_H_

#include <lua.h>
#include <type_traits>
#include <list>
#include <vector>
#include <map>
#include <pair>
#include <tuple>

#include <LunaWrapIsA.h>
#include <LunaWrapPush.h>
#include <LunaWrapCheck.h>

// Define that, if you want an std::pair to be pushed as { 1 = first , 2 = second , "first" = first , "second" = second }
// instead of just { "first" = first , "second" = second }
#undef _LUNA_WRAP_CONFIG_LUA_PAIR_AS_ARRAY_

namespace LunaWrap
{
	template<typename T>
	using remove_ref = typename std::remove_reference<T>::type;
	
	template<typename T>
	using remove_const = typename std::remove_const<T>::type;
	
	template<typename T>
	using helperType1 = typename std::enable_if< (std::is_reference<T>::value && std::is_const<remove_ref<T>>::value)>::type;
	
	template<typename T>
	using helperType2 = typename std::enable_if<(!std::is_reference<T>::value)>::type;
	
	// ~~~~~~~~~~~~~~~~~ Front-End Methods ~~~~~~~~~~~~~~~~~
	
	//! Check if a stack object is of the specified type
	template<
		typename T
		, typename T2 = remove_const< remove_ref<T> >
	>
	static inline bool is_a( lua_State* state , int index ){
		return LunaWrap::detail::is_a( state , index , (T2*)nullptr );
	}
	
	//! Check if a stack object is of a specific class (determined by its class-name)
	static inline bool	is_a( lua_State* state , int index , _literal className )
	{
		if( lua_getmetatable( state , index ) )		// Does it have a metatable?
		{
			luaL_getmetatable( state , className );	// Get correct metatable
			if( lua_rawequal( state , -1 , -2 ) ){	// Are they equal?
				lua_pop(state, 2);					// Remove both metatables
				return true;
			}
			lua_pop(state, 2);						// Remove both metatables
		}
		return false;
	}
	
	//! check<T> for const-reference types
	template<typename T>
	static inline auto check( lua_State* state , int index , helperType1<T>* = nullptr )
		-> decltype( LunaWrap::detail::check( state , index , (remove_const<remove_ref<T>>*)nullptr ) )
	{
		return LunaWrap::detail::check( state , index , (remove_const<remove_ref<T>>*)nullptr );
	}
	
	//! check<T> for non-reference types
	template<typename T>
	static inline auto check( lua_State* state , int index , helperType2<T>* = nullptr )
		-> decltype( LunaWrap::detail::check( state , index , (remove_const<T>*)nullptr ) )
	{
		return LunaWrap::detail::check( state , index , (remove_const<T>*)nullptr );
	}
	
	//! Receive the object of type 'T' from the stack, if it doesn't exist, 'fallback' is returned
	template<
		typename T
		, typename Ret = decltype( LunaWrap::check<T>( nullptr , 0 ) )
		, typename ParamType = Ret
	>
	static Ret lightcheck( lua_State* state , int index , ParamType fallback = ParamType() ){
		if( LunaWrap::is_a<T>( state , index ) )
			return LunaWrap::check<T>( state , index );
		else if( !lua_isnoneornil( state , index ) ) // Wrong type supplied
			lua_notnoneerror( state , index );
		return move(fallback);
	}
}

#endif