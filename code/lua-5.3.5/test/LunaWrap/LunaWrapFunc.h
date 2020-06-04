#ifndef _LUNA_WRAP_FUNC_H_
#define _LUNA_WRAP_FUNC_H_

#include <tuple>
#include <type_traits>

#include <lua.h>
#include <LunaWrap.h>

namespace LunaWrapFuncHelper
{
	template<size_t index,typename... Types>
	using nth_type
		= typename std::tuple_element<index, std::tuple<Types...>>::type;
	
	template<
		class ProxyClass,			typename WrappedFuncType,
		WrappedFuncType funcToWrap,	typename Return,
		typename Class,				typename... Args
	>
	class wrapFunctionImpl
	{
		private:
			
			typedef Return (Class::*wrappedFuncType)(Args...);
			typedef int (ProxyClass::*proxyFuncType)(lua_State*);
			
			// Dispatcher for void-functions
			template<
				size_t curIndex , size_t maxIndex
				, class... CollectedArgs 
			>
			inline typename std::enable_if<( curIndex == maxIndex && std::is_void<Return>::value ),int>::type
				call_with_params( lua_State* state , ProxyClass& instance , CollectedArgs... params)
			{
				Class* dest = &((Class&)instance); // Converts the proxyclass to the destination class
				(dest->*funcToWrap)(std::move(params)...);
				return 0;
			}
			
			// Dispatcher for non-void-returns that are no references that can/should be moved
			template<
				size_t curIndex , size_t maxIndex
				, class... CollectedArgs
			>
			inline typename std::enable_if<
				(
					curIndex == maxIndex
					&& !std::is_void<Return>::value
					&& !std::is_lvalue_reference<Return>::value
				)
			,int>::type
				call_with_params( lua_State* state , ProxyClass& instance , CollectedArgs... params)
			{
				Class* dest = &((Class&)instance); // Converts the proxyclass to the destination class
				LunaWrap::push(
					state
					, (Return&&) (dest->*funcToWrap)(std::move(params)...)
				);
				return 1;
			}
			
			// Dispatcher for non-void-returns, that are references
			template<
				size_t curIndex , size_t maxIndex
				, class... CollectedArgs
			>
			inline typename std::enable_if<
				(
					curIndex == maxIndex
					&& !std::is_void<Return>::value
					&& std::is_lvalue_reference<Return>::value
				)
			,int>::type
				call_with_params( lua_State* state , ProxyClass& instance , CollectedArgs... params)
			{
				Class* dest = &((Class&)instance); // Converts the proxyclass to the destination class
				LunaWrap::push(
					state
					, (dest->*funcToWrap)(std::move(params)...)
				);
				return 1;
			}
			
			template<
				size_t curIndex			, size_t maxIndex
				, class... ParamTypes	, class... CollectedArgs
			>
			inline typename std::enable_if<( curIndex < maxIndex ),int>::type
				call_with_params( lua_State* state , ProxyClass& instance , CollectedArgs... params )
			{
				typedef nth_type<curIndex,ParamTypes...> curType;
				return
					this->call_with_params<curIndex+1,maxIndex,ParamTypes...>(
						state
						, instance
						, std::move(params)...
						, LunaWrap::check<curType>(state, curIndex+1 )
					);
			}
		
		public:
			
			// Method that will dispatch all calls to the wrapped function
			int dispatcher( lua_State* state ){
				return this->call_with_params<0,sizeof...(Args), Args...>( state , *reinterpret_cast<ProxyClass*>(this) );
			}
			
			operator proxyFuncType(){
				return reinterpret_cast<proxyFuncType>(&wrapFunctionImpl::dispatcher);
			}
	};
	
	template<
		class ProxyClass,		typename FuncType,
		FuncType funcToWrap,	typename Class,
		typename Return,		typename... Args
	>
	inline constexpr auto wrapFunctionHelper( Return (Class::*dummy)(Args...) )
		-> wrapFunctionImpl<ProxyClass, FuncType, funcToWrap, Return, Class, Args...>
	{
		return wrapFunctionImpl<ProxyClass, FuncType, funcToWrap, Return, Class, Args...>();
	}

	template<
		class ProxyClass,		typename FuncType,
		FuncType funcToWrap,	typename Class,
		typename Return,		typename... Args
	>
	inline constexpr auto wrapFunctionHelper( Return (Class::*dummy)(Args...) const )
		-> wrapFunctionImpl<ProxyClass, FuncType, funcToWrap, Return, Class, Args...>
	{
		return wrapFunctionImpl<ProxyClass, FuncType, funcToWrap, Return, Class, Args...>();
	}
	
	// Ignore the return type, if the return type is a reference to the object itself
	template<
		class ProxyClass,		typename FuncType,
		FuncType funcToWrap,	typename Class,
		typename... Args
	>
	inline constexpr auto wrapFunctionHelper( const Class& (Class::*dummy)(Args...) const )
		-> wrapFunctionImpl<ProxyClass, FuncType, funcToWrap, void, Class, Args...>
	{
		return wrapFunctionImpl<ProxyClass, FuncType, funcToWrap, void, Class, Args...>();
	}
	
	template<
		class ProxyClass,		typename FuncType,
		FuncType funcToWrap,	typename Class,
		typename... Args
	>
	inline constexpr auto wrapFunctionHelper( Class (Class::*dummy)(Args...) )
		-> wrapFunctionImpl<ProxyClass, FuncType, funcToWrap, void, Class, Args...>
	{
		return wrapFunctionImpl<ProxyClass, FuncType, funcToWrap, void, Class, Args...>();
	}
	
	
	//
	// Without Proxy-Class
	//
	template<
		typename FuncType,		FuncType funcToWrap,
		typename Class,			typename Return,
		typename... Args
	>
	inline constexpr auto wrapFunctionHelperNoProxy( Return (Class::*dummy)(Args...) )
		-> wrapFunctionImpl<Class, FuncType, funcToWrap, Return, Class, Args...>
	{
		return wrapFunctionImpl<Class, FuncType, funcToWrap, Return, Class, Args...>();
	}

	template<
		typename FuncType,		FuncType funcToWrap,
		typename Class,
		typename Return,		typename... Args
	>
	inline constexpr auto wrapFunctionHelperNoProxy( Return (Class::*dummy)(Args...) const )
		-> wrapFunctionImpl<Class, FuncType, funcToWrap, Return, Class, Args...>
	{
		return wrapFunctionImpl<Class, FuncType, funcToWrap, Return, Class, Args...>();
	}
	
	// Ignore the return type, if the return type is a reference to the object itself
	template<
		typename FuncType,		FuncType funcToWrap,
		typename Class,			typename... Args
	>
	inline constexpr auto wrapFunctionHelperNoProxy( const Class& (Class::*dummy)(Args...) const )
		-> wrapFunctionImpl<Class, FuncType, funcToWrap, void, Class, Args...>
	{
		return wrapFunctionImpl<Class, FuncType, funcToWrap, void, Class, Args...>();
	}
	
	template<
		typename FuncType,		FuncType funcToWrap,
		typename Class,			typename... Args
	>
	inline constexpr auto wrapFunctionHelperNoProxy( Class (Class::*dummy)(Args...) )
		-> wrapFunctionImpl<Class, FuncType, funcToWrap, void, Class, Args...>
	{
		return wrapFunctionImpl<Class, FuncType, funcToWrap, void, Class, Args...>();
	}
}

#define LunaWrapFunc(func) LunaWrapFuncHelper::wrapFunctionHelperNoProxy<decltype(func), (func)>(func)
#define LunaWrapFunc(func, proxy) LunaWrapFuncHelper::wrapFunctionHelper<proxy, decltype(func), (func)>(func)

#endif