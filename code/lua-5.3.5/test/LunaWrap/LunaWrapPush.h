#ifndef _LUNA_WRAP_H_
	#include <LunaWrap.h>
#endif

#ifndef _LUNA_WRAP_PUSH_H_
#define _LUNA_WRAP_PUSH_H_

#include <limits>
#include <type_traits>

namespace LunaWrap
{
	// ~~~~~~~~~~~~~~~~~~ Predefines ~~~~~~~~~~~~~~~~~~
	template<int currentIndex, typename... ValueType> void	pushTupleElementsInternal( lua_State* state , const std::tuple<ValueType...>& arg , typename std::enable_if<currentIndex,void*>::type = nullptr );
	template<int currentIndex, typename... ValueType> void	pushTupleElementsInternal( lua_State* state , const std::tuple<ValueType...>& arg , typename std::enable_if<!currentIndex,void*>::type = nullptr );
	template<typename ContainerType> void					pushContainer( lua_State* state , const ContainerType& arg );
	template<typename ContainerType> void					pushAssociativeContainer( lua_State* state , const ContainerType& arg , typename std::enable_if<std::numeric_limits<typename ContainerType::key_type>::is_integer,void*>::type = nullptr );
	template<typename ContainerType> void					pushAssociativeContainer( lua_State* state , const ContainerType& arg , typename std::enable_if<!std::numeric_limits<typename ContainerType::key_type>::is_integer,void*>::type = nullptr );
	template<typename... T, typename... TN> int				push( lua_State* state , const std::tuple<T...>& arg , TN... args );
	template<typename T1, typename T2, typename... TN> int	push( lua_State* state , const std::pair<T1,T2>& arg , TN... args );
	
	
	
	// ~~~~~~~~~~~~~~~~~~ Basic Types ~~~~~~~~~~~~~~~~~~
	inline int push(lua_State*){ return 0; } // Standard fallback
	template<typename... TN>	inline int push( lua_State* state , decltype(std::ignore)		, TN... args){ lua_pushnil( state ); return 1 + push( state , forward<TN>(args)... ); }
	template<typename = std::enable_if<std::numeric_limits<typename ContainerType::key_type>::is_integer::type, typename... TN>
								inline int push( lua_State* state , T arg						, TN... args){ lua_pushinteger( state , arg ); return 1 + push( state , forward<TN>(args)... ); }
	template<typename... TN>	inline int push( lua_State* state , const char* arg				, TN... args){ lua_pushstring( state , arg ); return 1 + push( state , forward<TN>(args)... ); }
	template<typename... TN>	inline int push( lua_State* state , bool arg					, TN... args){ lua_pushboolean( state , arg ); return 1 + push( state , forward<TN>(args)... ); }
	template<typename... TN>	inline int push( lua_State* state , float arg					, TN... args){ lua_pushnumber( state , arg ); return 1 + push( state , forward<TN>(args)... ); }
	template<typename... TN>	inline int push( lua_State* state , double arg					, TN... args){ lua_pushnumber( state , arg ); return 1 + push( state , forward<TN>(args)... ); }
	template<typename... TN>	inline int push( lua_State* state , const std::string& arg			, TN... args){ lua_pushstring( state , arg.c_str() ); return 1 + push( state , forward<TN>(args)... ); }
	
	
	// ~~~~~~~~~~~~~~~~~~ Normal Containers ~~~~~~~~~~~~~~~~~~
	template<typename T, typename... TN>
	inline int push( lua_State* state , const std::vector<T>& arg , TN... args ){ pushContainer( state , arg ); return 1 + push( state , forward<TN>(args)... ); }
	template<typename T, typename... TN>
	inline int push( lua_State* state , const std::list<T>& arg , TN... args ){ pushContainer( state , arg ); return 1 + push( state , forward<TN>(args)... ); }
	
	
	
	// ~~~~~~~~~~~~~~~~~~ Associative Containers ~~~~~~~~~~~~~~~~~~
	template<typename T, typename T2, typename... TN>
	inline int push( lua_State* state , const std::map<T,T2>& arg , TN... args ){ pushAssociativeContainer( state , arg ); return 1 + push( state , forward<TN>(args)... ); }
	
	
	// ~~~~~~~~~~~~~~~~~~ Pair & Tuple ~~~~~~~~~~~~~~~~~~
	template<typename T1, typename T2, typename... TN>
	inline int push( lua_State* state , const std::pair<T1,T2>& arg , TN... args )
	{
		#ifdef _LUNA_WRAP_CONFIG_LUA_PAIR_AS_ARRAY_
		lua_createtable( state , 2 , 2 );	// Create table
		push( state , move(arg.first) );	// Push First Value
		lua_rawseti( state , -2 , 1 );		// Add to table
		push( state , move(arg.second) );	// Push Second Value
		lua_rawseti( state , -2 , 2 );		// Add to table
		#else
		lua_createtable( state , 0 , 2 );	// Create table
		#endif
		lua_pushliteral( state , "first" );	// Push Key of first value
		push( state , move(arg.first) );	// Push First Value
		lua_rawset( state , -3 );			// Add to table
		lua_pushliteral( state , "second" );// Push Key of second Value
		push( state , move(arg.second) );	// Push Second Value
		lua_rawset( state , -3 );			// Add to table
		
		return push( state , args... ) + 1;
	}
	
	template<typename... T, typename... TN>
	inline int push( lua_State* state , const std::tuple<T...>& arg , TN... args )
	{
		lua_createtable( state , sizeof...(T) , 0 ); // Create table
		pushTupleElementsInternal<sizeof...(T)>( state , arg );
		return push( state , args... ) + 1;
	}
	
	
	
	// ~~~~~~~~~~~~~~~~~~ Internal Push-Functions ~~~~~~~~~~~~~~~~~~
	template<int currentIndex, typename... ValueType>
	inline void pushTupleElementsInternal( lua_State* state , const std::tuple<ValueType...>& arg , typename std::enable_if<currentIndex,void*>::type )
	{
		push( state , std::get<currentIndex-1>( arg ) );
		lua_rawseti( state , -2 , currentIndex );
		
		pushTupleElementsInternal<currentIndex-1>( state , arg );
	}
	template<int currentIndex, typename... ValueType>
	inline void pushTupleElementsInternal( lua_State* state , const std::tuple<ValueType...>& arg , typename std::enable_if<!currentIndex,void*>::type )
	{}
	
	template<typename ContainerType>
	void pushAssociativeContainer(
		lua_State* state
		, const ContainerType& arg
		, typename std::enable_if<!std::numeric_limits<typename ContainerType::key_type>::is_integer,void*>::type
	)
	{
		// Create Table for arguments
		lua_createtable( state , 0 , arg.size() );
		
		// Push Datasets
		int i = 0;
		for( const typename ContainerType::value_type& data : arg ){
			push( state , data );
			lua_rawseti( state , -2 , ++i );
		}
	}
	
	template<typename ContainerType>
	void pushAssociativeContainer(
		lua_State* state
		, const ContainerType& arg
		, typename std::enable_if<std::numeric_limits<typename ContainerType::key_type>::is_integer,void*>::type
	)
	{
		// Create Table for arguments
		lua_createtable( state , 0 , arg.size() );
		
		// Push Datasets
		for( const typename ContainerType::value_type& data : arg ){
			push( state , data.second );
			lua_rawseti( state , -2 , data.first );
		}
	}
	
	template<typename ContainerType>
	void pushContainer( lua_State* state , const ContainerType& arg )
	{
		// Create Table for arguments
		lua_createtable( state , arg.size() , 0 );
		
		// Push Datasets
		int i = 0;
		for( const typename ContainerType::value_type& data : arg ){
			push( state , data );
			lua_rawseti( state , -2 , ++i );
		}
	}
}

#endif