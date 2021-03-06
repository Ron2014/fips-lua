#ifndef _LUNA_WRAP_H_
	#include <LunaWrap.h>
#endif

#ifndef _LUNA_WRAP_IS_A_H_
#define _LUNA_WRAP_IS_A_H_

namespace LunaWrap
{
	namespace detail
	{
		// Get the type of a lua index
		static inline int	get_type( lua_State* state , int index ){ return lua_type( state , index ); }
		
		// Check if the value at the given index of the lua stack is of the supplied type
		static inline bool	is_a( lua_State* state , int index , int luaType ){ return get_type( state , index ) == luaType; }
		
		// Nothing to check for (neccessary for return-values of wrapped c-functions)
		static inline bool	is_a( lua_State* state , int index , void* dummy){ return true; }
		
		// ~~~~~~~~~~~~~~~~~~ Basic Types ~~~~~~~~~~~~~~~~~~
		static inline bool	is_a( lua_State* state , int index ,					char*	dummy){ return is_a( state , index , LUA_TNUMBER ); }
		static inline bool	is_a( lua_State* state , int index , 					int*	dummy){ return is_a( state , index , LUA_TNUMBER ); }
		static inline bool	is_a( lua_State* state , int index , short				int*	dummy){ return is_a( state , index , LUA_TNUMBER ); }
		static inline bool	is_a( lua_State* state , int index , long				int*	dummy){ return is_a( state , index , LUA_TNUMBER ); }
		static inline bool	is_a( lua_State* state , int index , long long			int*	dummy){ return is_a( state , index , LUA_TNUMBER ); }
		static inline bool	is_a( lua_State* state , int index , unsigned			char*	dummy){ return is_a( state , index , LUA_TNUMBER ); }
		static inline bool	is_a( lua_State* state , int index , unsigned			int*	dummy){ return is_a( state , index , LUA_TNUMBER ); }
		static inline bool	is_a( lua_State* state , int index , unsigned short		int*	dummy){ return is_a( state , index , LUA_TNUMBER ); }
		static inline bool	is_a( lua_State* state , int index , unsigned long		int*	dummy){ return is_a( state , index , LUA_TNUMBER ); }
		static inline bool	is_a( lua_State* state , int index , unsigned long long	int*	dummy){ return is_a( state , index , LUA_TNUMBER ); }
		static inline bool	is_a( lua_State* state , int index , bool* ){
			int type = get_type( state , index );
			return type == LUA_TBOOLEAN || type == LUA_TNUMBER;
		}
		static inline bool	is_a( lua_State* state , int index , const char* ){ return is_a( state , index , LUA_TSTRING ); }
		static inline bool	is_a( lua_State* state , int index , std::string* ){ return is_a( state , index , LUA_TSTRING ); }
		
		// ~~~~~~~~~~~~~~~~~~ Normal Containers ~~~~~~~~~~~~~~~~~~
		template<typename T>
		static inline bool	is_a( lua_State* state , int index , std::list<T>* ){ return lua_istable( state , index ); }
		template<typename T>
		static inline bool	is_a( lua_State* state , int index , std::vector<T>* ){ return lua_istable( state , index ); }
		static bool checkIfPair( lua_State* state , int index ){
			if( !lua_istable( state , index ) )
				return false;
			
			// Convert all relative indices to absolute since they would be invalidated after lua_rawgeti
			index = lua_toAbsIndex( state , index );
			
			lua_rawgetfield( state , index , "first" );
			if( lua_isnil( state , -1 ) )
				goto notPair;
			
			lua_pop( state , 1 );
			
			lua_rawgetfield( state , index , "second" );
			if( lua_isnil( state , -1 ) )
				goto notPair;
			
			lua_pop( state , 1 );
			return true;
			
			notPair:
			lua_pop( state , 1 );
			return false;
		}
		template<typename T1, typename T2>
		static inline bool	is_a( lua_State* state , int index , std::pair<T1,T2>* ){
			return checkIfPair( state , index );
		}
		
		// ~~~~~~~~~~~~~~~~~~ Associative Containers ~~~~~~~~~~~~~~~~~~
		template<typename T1, typename T2>
		static inline bool	is_a( lua_State* state , int index , std::map<T1,T2>* ){ return lua_istable( state , index ); }
		
		// ~~~~~~~~~~~~~~~~~~ Luna-Proxy-Classes ~~~~~~~~~~~~~~~~~~
		template<typename T>
		static inline bool	is_a( lua_State* state , int index , T* dummy ){ return T::test( state , index ); }
	}
}

#endif