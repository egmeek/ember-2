/*
 Copyright (C) 2009 Erik Ogenvik <erik@ogenvik.org>

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */


#ifndef EMBEROGRETERRAINOGREIMAGE_H_
#define EMBEROGRETERRAINOGREIMAGE_H_

#include "Image.h"

namespace Ember
{
namespace OgreView
{

namespace Terrain
{

class WFImage;

class OgreImage : public Image
{
public:
	OgreImage(Image::ImageBuffer* buffer);

	void blit(const OgreImage& imageToBlit, unsigned int destinationChannel, int widthOffset = 0, int heightOffset = 0);
	void blit(const WFImage& imageToBlit, unsigned int destinationChannel, int widthOffset = 0, int heightOffset = 0);

};

}

}

}

#endif /* EMBEROGRETERRAINOGREIMAGE_H_ */
