#ifndef _LUNA_WRAP_H_
	#include <LunaWrap.h>
#endif

#ifndef _LUNA_WRAP_CHECK_H_
#define _LUNA_WRAP_CHECK_H_

namespace LunaWrap
{
	namespace detail
	{
		// ~~~~~~~~~~~~~~~~~~ Predefines ~~~~~~~~~~~~~~~~~~
		template<
			typename ContainerType
			, typename ValueType = typename ContainerType::value_type
		>	static ContainerType				checkContainerInternal( lua_State* state , int index );
		
		template<
			typename ContainerType
			, typename KeyType = typename ContainerType::key_type
			, typename ValueType = typename ContainerType::mapped_type
		>	static ContainerType				checkAssociativeContainerInternal( lua_State* state , int index );
		
		template<
			typename PairType
			, typename FirstType = typename PairType::first_type
			, typename SecondType = typename PairType::second_type
		>	static PairType					checkPairInternal( lua_State* state , int index );
		
		template<
			int countDown
			, int currentLuaIndex
			, typename CurrentType
			, typename... TupleTypes
		>	static inline
			std::tuple<CurrentType,TupleTypes...>		checkTupleElementsInternal( lua_State* state , int index , typename std::enable_if<countDown,void*>::type = nullptr );
		
		template<
			int countDown
			, int currentLuaIndex
		>	static inline std::tuple<>		checkTupleElementsInternal( lua_State* state , int , typename std::enable_if<!countDown,void*>::type = nullptr );
		
		
		
		// ~~~~~~~~~~~~~~~~~~ Basic Types ~~~~~~~~~~~~~~~~~~
		static inline void					check( lua_State* state , int index , void* ){}
		template<typename T, typename = std::enable_if<std::numeric_limits<typename ContainerType::key_type>::is_integer::type>
		static inline auto					check( lua_State* state , int index , T* ) -> T { return luaL_checkinteger( state , index ); }
		static inline float					check( lua_State* state , int index , float* ){ return luaL_checknumber( state , index ); }
		static inline double				check( lua_State* state , int index , double* ){ return luaL_checknumber( state , index ); }
		static inline bool					check( lua_State* state , int index , bool* ){
			if( lua_isboolean( state , index ) )
				return lua_toboolean( state , index );
			return luaL_checkinteger( state , index );
		}
		static inline char					check( lua_State* state , int index , char* ){
			if( lua_isstring( state , index ) )
				return luaL_checkstring( state , index )[0];
			return luaL_checkinteger( state , index );
		}
		static inline std::string			check( lua_State* state , int index , std::string* ){ const char* str = luaL_checkstring( state , index ); return str ? std::string(str) : string(); }
		static inline const char*			check( lua_State* state , int index , const char** ){ return luaL_checkstring( state , index ); }
		
		
		
		// ~~~~~~~~~~~~~~~~~~ Tuples ~~~~~~~~~~~~~~~~~~
		template<typename... T>
		static inline std::tuple<T...>			check( lua_State* state , int index , std::tuple<T...>* arg )
		{
			// Check if table is present
			if( !lua_istable( state , index ) )
				lua_tagerror( state , index , LUA_TTABLE );
			
			return checkTupleElementsInternal<sizeof...(T),1,T...>( state , index );
		}
		template<
			int countDown
			, int currentLuaIndex
			, typename CurrentType
			, typename... TupleTypes
		>
		static inline
			std::tuple<CurrentType,TupleTypes...>		checkTupleElementsInternal( lua_State* state , int index , typename std::enable_if<countDown,void*>::type )
		{
			lua_rawgeti( state , index , currentLuaIndex ); // Get element at current index in table
			
			// Read the current index from the lua table
			CurrentType current = check( state , -1 , (CurrentType*)nullptr );
			
			// Pop Value
			lua_pop( state , 1 );
			
			// Return concatenated tuple
			return std::tuple_cat(
				makestd::tuple( move(current) )
				, checkTupleElementsInternal<
					countDown - 1
					, currentLuaIndex + 1
					, TupleTypes...
				>( state , index )
			);
		}
		template<
			int countDown
			, int currentLuaIndex
		>
		static inline std::tuple<>				checkTupleElementsInternal( lua_State* state , int , typename std::enable_if<!countDown,void*>::type )
		{
			return std::tuple<>();
		}
		
		
		
		// ~~~~~~~~~~~~~~~~~~ Normal Containers ~~~~~~~~~~~~~~~~~~
		template<typename T>
		static inline std::list<T>				check( lua_State* state , int index , std::list<T>* ){
			return checkContainerInternal<std::list<T>>( state , index );
		}
		template<typename T>
		static inline std::vector<T>				check( lua_State* state , int index , std::vector<T>* ){
			return checkContainerInternal<std::vector<T>>( state , index );
		}
		template<typename T1, typename T2>
		static inline std::pair<T1,T2>			check( lua_State* state , int index , std::pair<T1,T2>* ){
			return checkPairInternal<std::pair<T1,T2>>( state , index );
		}
		
		
		
		// ~~~~~~~~~~~~~~~~~~ Associative Containers ~~~~~~~~~~~~~~~~~~
		template<typename T1, typename T2>
		static inline std::map<T1,T2>			check( lua_State* state , int index , std::map<T1,T2>* ){
			return checkAssociativeContainerInternal<std::map<T1,T2>>( state , index );
		}
		
		
		
		// ~~~~~~~~~~~~~~~~~~~ Internal Container Checks ~~~~~~~~~~~~~~~~~~~
		template<
			typename ContainerType
			, typename ValueType = typename ContainerType::value_type
		>
		static ContainerType checkContainerInternal( lua_State* state , int index )
		{
			// Check if table is present
			if( !lua_istable( state , index ) )
				lua_tagerror( state , index , LUA_TTABLE );
			
			// Convert all relative indices to absolute since they would be invalidated after lua_rawgeti
			index = lua_toAbsIndex( state , index );
			
			ContainerType ret;
			for( int i = 1; ; i++ )
			{
				lua_rawgeti( state , index , i ); // Get next index in table
				
				if( lua_isnil( state , -1 ) ){
					lua_pop( state , 1 ); // Pop Value
					break;
				}
				ret.push_back( check( state , -1 , (ValueType*)nullptr ) );
				
				lua_pop( state , 1 );// Pop Value
			}
			return ret;
		}
		
		template<
			typename ContainerType
			, typename KeyType = typename ContainerType::key_type
			, typename ValueType = typename ContainerType::mapped_type
		>
		static ContainerType checkAssociativeContainerInternal( lua_State* state , int index )
		{
			// Check if table is present
			if( !lua_istable( state , index ) )
				lua_tagerror( state , index , LUA_TTABLE );
			
			// Convert all relative indices to absolute since they would be invalidated after lua_rawgeti
			index = lua_toAbsIndex( state , index );
			
			ContainerType ret;
			
			lua_pushnil( state );  // first key
			
			while( lua_next( state , index ) != 0 )
			{
				// Copy key, in case some function calls lua_tostring, which would break the lua_next-iteration
				lua_pushvalue( state , -2 );
				
				// uses copy of 'key' (at index -1) and 'value' (at index -2)
				ret[ check( state , -1 , (KeyType*)nullptr ) ] = check( state , -2 , (ValueType*)nullptr );
				
				// removes 'value' and the copy of 'key' ; keeps original 'key' for next iteration
				lua_pop( state , 2 );
			}
			
			return ret;
		}
		
		template<
			typename PairType
			, typename FirstType = typename PairType::first_type
			, typename SecondType = typename PairType::second_type
		>
		static PairType checkPairInternal( lua_State* state , int index )
		{
			// Check if table is present
			if( !lua_istable( state , index ) )
				lua_tagerror( state , index , LUA_TTABLE );
			
			// Convert all relative indices to absolute since they would be invalidated after lua_rawgeti
			index = lua_toAbsIndex( state , index );
			
			lua_rawgetfield( state , index , "second" ); // Get second value
			
			#ifdef _LUNA_WRAP_CONFIG_LUA_PAIR_AS_ARRAY_
			// Check if the pair consists of .second and .first
			if( lua_isnil( state , -1 ) ){
				lua_pop( state , 1 );
				lua_rawgeti( state , index , 2 );	// Get second value
				lua_rawgeti( state , index , 1 );	// Get first value
			}
			else
			#endif
				lua_rawgetfield( state , index , "first" );		// Get first value
			
			// [-1] = First
			// [-2] = Second
			PairType ret = PairType(
				check( state , -1 , (FirstType*)nullptr ) ,
				check( state , -2 , (SecondType*)nullptr )
			);
			
			// Pop first/second value
			lua_pop( state , 2 );
			
			return move(ret);
		}
	}
}

#endif