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

#include "OceanAction.h"
#include "OceanRepresentation.h"
#include "Environment.h"

#include "domain/EmberEntity.h"
#include "components/ogre/EmberOgre.h"
#include "components/ogre/World.h"

namespace Ember
{
namespace OgreView
{
namespace Environment
{
OceanAction::OceanAction(EmberEntity& entity) :
		mEntity(entity)
{
}

OceanAction::~OceanAction()
{
}

void OceanAction::activate(EntityMapping::ChangeContext& context)
{
	if (EmberOgre::getSingleton().getWorld()) {
		World* world = EmberOgre::getSingleton().getWorld();
		if (Environment* environment = world->getEnvironment()) {
			OceanRepresentation* ocean = new OceanRepresentation(mEntity, *environment);
			mEntity.setGraphicalRepresentation(ocean);
			return;
		} else {
			S_LOG_WARNING("Tried to activate ocean representation, but there was no world instance available.");
		}
	}
}

void OceanAction::deactivate(EntityMapping::ChangeContext& context)
{
	mEntity.setGraphicalRepresentation(0);
}
}
}
}
