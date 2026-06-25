/*
 *
 * Copyright 2020-2021 Apple Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//-------------------------------------------------------------------------------------------------------------------------------------------------------------
//
// AppKit/NSView.hpp
//
//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma once

//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#include "AppKitPrivate.hpp"
#include <Foundation/NSObject.hpp>
#include <CoreGraphics/CGGeometry.h>
#include <cstdint>

namespace NS
{
	class View : public NS::Referencing< View >
	{
		public:
			View*		init( CGRect frame );

			// View hierarchy
			void		addSubview( const View* pSubview );
			CGRect		bounds() const;
			void		setFrame( CGRect frame );
			void		setAutoresizingMask( uint32_t mask );
			void		setNeedsDisplay( bool needsDisplay );

			// Returns true if the receiver is an instance of (or subclass of) pClass.
			bool		isKindOfClass( const Class* pClass ) const;
	};
}


_NS_INLINE NS::View* NS::View::init( CGRect frame )
{
	return Object::sendMessage< View* >( _APPKIT_PRIVATE_CLS( NSView ), _APPKIT_PRIVATE_SEL( initWithFrame_ ), frame );
}

_NS_INLINE void NS::View::addSubview( const NS::View* pSubview )
{
	Object::sendMessage< void >( this, _APPKIT_PRIVATE_SEL( addSubview_ ), pSubview );
}

_NS_INLINE CGRect NS::View::bounds() const
{
	return Object::sendMessage< CGRect >( this, _APPKIT_PRIVATE_SEL( bounds ) );
}

_NS_INLINE void NS::View::setFrame( CGRect frame )
{
	Object::sendMessage< void >( this, _APPKIT_PRIVATE_SEL( setFrame_ ), frame );
}

_NS_INLINE void NS::View::setAutoresizingMask( uint32_t mask )
{
	Object::sendMessage< void >( this, _APPKIT_PRIVATE_SEL( setAutoresizingMask_ ), mask );
}

_NS_INLINE void NS::View::setNeedsDisplay( bool needsDisplay )
{
	Object::sendMessage< void >( this, _APPKIT_PRIVATE_SEL( setNeedsDisplay_ ), needsDisplay );
}

_NS_INLINE bool NS::View::isKindOfClass( const Class* pClass ) const
{
	return Object::sendMessage< bool >( this, _APPKIT_PRIVATE_SEL( isKindOfClass_ ), pClass );
}
