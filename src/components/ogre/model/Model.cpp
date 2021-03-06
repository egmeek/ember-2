/*
 Copyright (C) 2004  Erik Ogenvik
 Copyright (c) 2005 The Cataclysmos Team

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "Model.h"
#include "SubModel.h"
#include "SubModelPart.h"
#include "ParticleSystemBinding.h"

#include "components/ogre/EmberOgre.h"
#include "ModelDefinitionManager.h"
#include "ModelBackgroundLoader.h"


#include "framework/TimeFrame.h"
#include "framework/TimedLog.h"

#include <OgreTagPoint.h>
#include <OgreMeshManager.h>
#include <OgreSceneManager.h>
#include <OgreSubEntity.h>
#include <OgreParticleSystem.h>
#include <OgreParticleEmitter.h>

namespace Ember
{
namespace OgreView
{
namespace Model
{

const Ogre::String Model::sMovableType = "Model";
unsigned long Model::msAutoGenId = 0;

Model::Model(const std::string& name) :
	Ogre::MovableObject(name), mSkeletonOwnerEntity(0), mSkeletonInstance(0), mScale(0.0f), mRotation(Ogre::Quaternion::IDENTITY), mAnimationStateSet(0), mAttachPoints(nullptr), mBackgroundLoader(0)
{
	mVisible = true;
}
Model::~Model()
{
	if (mBackgroundLoader) {
		mBackgroundLoader->detachFromModel();
	}
	if (mSkeletonOwnerEntity) {
		mSkeletonOwnerEntity->getMesh()->removeListener(this);
	}
	resetSubmodels();
	resetParticles();
	resetLights();
	if (!mDefinition.isNull()) {
		mDefinition->removeModelInstance(this);
	}
	S_LOG_VERBOSE("Deleted "<< getName());
}

void Model::reset()
{
	S_LOG_VERBOSE("Resetting "<< getName());
	Resetting.emit();
	if (mSkeletonOwnerEntity) {
		mSkeletonOwnerEntity->getMesh()->removeListener(this);
	}
	//	resetAnimations();
	resetSubmodels();
	resetParticles();
	resetLights();
	mScale = 0;
	mRotation = Ogre::Quaternion::IDENTITY;
	mSkeletonInstance = 0;
	// , mAnimationStateSet(0)
	mSkeletonOwnerEntity = 0;
	mAttachPoints = std::unique_ptr<AttachPointWrapperStore>(nullptr);

}

void Model::reload()
{
	//	resetAnimations();
	/*	resetSubmodels();
	 resetParticles();	*/
	reset();
	createFromDefn();
	//if we are attached, we have to notify the new entities, else they won't appear in the scene
	_notifyAttached(mParentNode, mParentIsTagPoint);

	Reloaded.emit();
}

bool Model::create(const std::string& modelType, Eris::EventService& eventService)
{
	if (!mDefinition.isNull() && mDefinition->isValid()) {
		S_LOG_WARNING("Trying to call create('" + modelType + "') on a Model instance that already have been created as a '" + mDefinition->getName() + "'.");
		return false;
	}

	static const Ogre::String groupName("ModelDefinitions");
	//Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME;
	try {
		mDefinition = ModelDefinitionManager::getSingleton().load(modelType, groupName).staticCast<ModelDefinition>();
	} catch (const std::exception& ex) {
		S_LOG_FAILURE("Could not load model of type " << modelType << " from group " << groupName << "." << ex);
		return false;
	}
	if (false && !mDefinition->isValid()) {
		S_LOG_FAILURE("Model of type " << modelType << " from group " << groupName << " is not valid.");
		return false;
	} else {
#if OGRE_THREAD_SUPPORT
		if (!mBackgroundLoader) {
			mBackgroundLoader = std::make_shared<ModelBackgroundLoader>(this, eventService);
		}
#endif
		mDefinition->addModelInstance(this);
		return true;
		/*		bool success =  createFromDefn();
		 if (!success) {
		 reset();
		 }
		 return success;*/
	}
}

void Model::_notifyManager(Ogre::SceneManager* man)
{
	Ogre::MovableObject::_notifyManager(man);
	bool success = createFromDefn();
	if (!success) {
		reset();
	}
}

void Model::loadingComplete(Ogre::Resource*)
{
	//This is called when the mesh is reloaded and it has a skeleton; we need to reset the actions since they now refer to invalid animation states

	for (auto& subModel : mSubmodels) {
		//We need to call _initialise in order for the animation states to get recreated;
		//else this will happen lazily the next time the entity is rendered.
		subModel->getEntity()->_initialise(true);
	}

	for (auto& actionDef : mDefinition->getActionDefinitions()) {
		auto actionI = mActions.find(actionDef->getName());
		if (actionI != mActions.end()) {
			Action& action = actionI->second;
			//Important these calls happen in this order, else we'll risk segfaults
			action.getAnimations().getAnimations().clear();
			action.getAnimations().reset();

			for (auto& animationDef : actionDef->getAnimationDefinitions()) {
				Animation animation(animationDef->getIterations(), getSkeleton()->getNumBones());
				for (auto& animationPartDef : animationDef->getAnimationPartDefinitions()) {
					if (getAllAnimationStates()->hasAnimationState(animationPartDef->Name)) {
						AnimationPart animPart;
						try {
							Ogre::AnimationState* state = getAnimationState(animationPartDef->Name);
							animPart.state = state;
							for (auto& boneGroupDef : animationPartDef->BoneGroupRefs) {
								auto I_boneGroup = mDefinition->getBoneGroupDefinitions().find(boneGroupDef.Name);
								if (I_boneGroup != mDefinition->getBoneGroupDefinitions().end()) {
									BoneGroupRef boneGroupRef;
									boneGroupRef.boneGroupDefinition = I_boneGroup->second;
									boneGroupRef.weight = boneGroupDef.Weight;
									animPart.boneGroupRefs.push_back(boneGroupRef);
								}
							}
							animation.addAnimationPart(animPart);
						} catch (const std::exception& ex) {
							S_LOG_FAILURE("Error when loading animation: " << animationPartDef->Name << "." << ex);
						}
					}
				}
				action.getAnimations().addAnimation(animation);
			}
		}
	}
}


bool Model::createFromDefn()
{
	TimedLog timedLog(std::string("Model::createFromDefn ") + getDefinition()->getName());
	// create instance of model from definition
	mScale = mDefinition->mScale;
	mRotation = mDefinition->mRotation;

#if OGRE_THREAD_SUPPORT
	if (mBackgroundLoader->poll()) {
		timedLog.report("Initial poll.");
		return createActualModel();
	}
	else {
		setRenderingDistance(mDefinition->getRenderingDistance());
		return true;
	}
#else
	return createActualModel();
#endif
}

bool Model::createActualModel()
{
	TimedLog timedLog(std::string("Model::createActualModel ") + getDefinition()->getName());
	Ogre::SceneManager* sceneManager = _getManager();
	std::vector<std::string> showPartVector;

	for (auto& submodelDef : mDefinition->getSubModelDefinitions()) {
		std::string entityName = mName + "/" + submodelDef->getMeshName();
		try {

			Ogre::Entity* entity = sceneManager->createEntity(entityName, submodelDef->getMeshName());
			timedLog.report("Created entity.");
			if (entity->getMesh().isNull()) {
				S_LOG_FAILURE("Could not load mesh " << submodelDef->getMeshName() << " which belongs to model " << mDefinition->getName() << ".");
			}

			if (mDefinition->getRenderingDistance() > .0f) {
				entity->setRenderingDistance(mDefinition->getRenderingDistance());
			}

			SubModel* submodel = new SubModel(*entity);
			//Model::SubModelPartMapping* submodelPartMapping = new Model::SubModelPartMapping();

			if (!submodelDef->getPartDefinitions().empty()) {
				for (auto& partDef : submodelDef->getPartDefinitions()) {
					SubModelPart& part = submodel->createSubModelPart(partDef->getName());
					//std::string groupName("");

					if (partDef->getSubEntityDefinitions().size() > 0) {
						for (auto& subEntityDef : partDef->getSubEntityDefinitions()) {
							try {
								Ogre::SubEntity* subEntity(0);
								//try with a submodelname first
								if (subEntityDef->getSubEntityName() != "") {
									subEntity = entity->getSubEntity(subEntityDef->getSubEntityName());
								} else {
									//no name specified, use the index instead
									if (entity->getNumSubEntities() > subEntityDef->getSubEntityIndex()) {
										subEntity = entity->getSubEntity(subEntityDef->getSubEntityIndex());
									} else {
										S_LOG_WARNING("Model definition " << mDefinition->getName() << " has a reference to entity with index " << subEntityDef->getSubEntityIndex() << " which is out of bounds.");
									}
								}
								if (subEntity) {
									part.addSubEntity(subEntity, subEntityDef);

									if (subEntityDef->getMaterialName() != "") {
										subEntity->setMaterialName(subEntityDef->getMaterialName());
									}
								} else {
									S_LOG_WARNING("Could not add subentity.");
								}
							} catch (const std::exception& ex) {
								S_LOG_WARNING("Error when getting sub entities for model '" << mDefinition->getName() << "'." << ex);
							}
						}
					} else {
						//if no subentities are defined, add all subentities
						unsigned int numSubEntities = entity->getNumSubEntities();
						for (unsigned int i = 0; i < numSubEntities; ++i) {
							part.addSubEntity(entity->getSubEntity(i), 0);
						}
					}
					if (partDef->getGroup() != "") {
						mGroupsToPartMap[partDef->getGroup()].push_back(partDef->getName());
						//mPartToGroupMap[partDef->getName()] = partDef->getGroup();
					}

					if (partDef->getShow()) {
						showPartVector.push_back(partDef->getName());
					}

					ModelPart& modelPart = mModelParts[partDef->getName()];
					modelPart.addSubModelPart(&part);
					modelPart.setGroupName(partDef->getGroup());
				}
			} else {
				//if no parts are defined, add a default "main" part and add all subentities to it. This ought to be a good default behaviour
				SubModelPart& part = submodel->createSubModelPart("main");
				for (size_t i = 0; i < entity->getNumSubEntities(); ++i) {
					Ogre::SubEntity* subentity = entity->getSubEntity(i);
					part.addSubEntity(subentity, nullptr);
				}
				showPartVector.push_back(part.getName());
				ModelPart& modelPart = mModelParts[part.getName()];
				modelPart.addSubModelPart(&part);
			}
			addSubmodel(submodel);
			timedLog.report("Created submodel.");
		} catch (const std::exception& e) {
			S_LOG_FAILURE( "Submodel load error for " << entityName << "." << e);
			return false;
		}
	}

	setRenderingDistance(mDefinition->getRenderingDistance());

	createActions();
	timedLog.report("Created actions.");

	createParticles();
	timedLog.report("Created particles.");

	createLights();
	timedLog.report("Created lights.");

	for (auto& part : showPartVector) {
		showPart(part);
	}
	return true;
}

void Model::createActions()
{

	//If the mesh has a skeleton we'll add a listener to the mesh, so that we can reload the animation states when
	//the mesh or skeleton gets reloaded.
	if (getSkeleton()) {
		mSkeletonOwnerEntity->getMesh()->addListener(this);
	}

	for (auto& actionDef : mDefinition->getActionDefinitions()) {
		Action action;
		action.setName(actionDef->getName());
		action.getAnimations().setSpeed(actionDef->getAnimationSpeed());

		if (getSkeleton() && getAllAnimationStates()) {

			if (!mDefinition->getBoneGroupDefinitions().empty()) {
				//If there are bone groups, we need to use a cumulative blend mode. Note that this will affect all animations in the model.
				getSkeleton()->setBlendMode(Ogre::ANIMBLEND_CUMULATIVE);
			}
			if (!mSubmodels.empty()) {
				for (auto& animationDef : actionDef->getAnimationDefinitions()) {
					Animation animation(animationDef->getIterations(), getSkeleton()->getNumBones());
					for (auto& animationPartDef : animationDef->getAnimationPartDefinitions()) {
						if (getAllAnimationStates()->hasAnimationState(animationPartDef->Name)) {
							AnimationPart animPart;
							try {
								Ogre::AnimationState* state = getAnimationState(animationPartDef->Name);
								animPart.state = state;
								for (auto& boneGroupDef : animationPartDef->BoneGroupRefs) {
									auto I_boneGroup = mDefinition->getBoneGroupDefinitions().find(boneGroupDef.Name);
									if (I_boneGroup != mDefinition->getBoneGroupDefinitions().end()) {
										BoneGroupRef boneGroupRef;
										boneGroupRef.boneGroupDefinition = I_boneGroup->second;
										boneGroupRef.weight = boneGroupDef.Weight;
										animPart.boneGroupRefs.push_back(boneGroupRef);
									}
								}
								animation.addAnimationPart(animPart);
							} catch (const std::exception& ex) {
								S_LOG_FAILURE("Error when loading animation: " << animationPartDef->Name << "." << ex);
							}
						}
					}
					action.getAnimations().addAnimation(animation);
				}
			}
		}

		//TODO: add sounds too

		mActions[actionDef->getName()] = action;
	}
}

void Model::createParticles()
{
	for (auto& particleSystemDef : mDefinition->mParticleSystems) {
		//first try to create the ogre particle system
		std::string name(mName + "/particle" + particleSystemDef.Script);
		Ogre::ParticleSystem* ogreParticleSystem;
		try {
			ogreParticleSystem = _getManager()->createParticleSystem(name, particleSystemDef.Script);
		} catch (const std::exception& ex) {
			S_LOG_FAILURE("Could not create particle system: " << name << "." << ex);
			continue;
		}
		if (ogreParticleSystem) {
			//ogreParticleSystem->setDefaultDimensions(1, 1);
			ParticleSystem* particleSystem = new ParticleSystem(ogreParticleSystem, particleSystemDef.Direction);
			for (auto& bindingDef : particleSystemDef.Bindings) {
				ParticleSystemBinding* binding = particleSystem->addBinding(bindingDef.EmitterVar, bindingDef.AtlasAttribute);
				mAllParticleSystemBindings.push_back(binding);
			}
			mParticleSystems.push_back(particleSystem);
			refreshMovableList();
		}

	}
}

void Model::createLights()
{
	int j = 0;
	for (auto& lightDef : mDefinition->mLights) {
		//first try to create the ogre lights
		//std::string name(mName + "/light");
		std::stringstream name;
		name << mName << "/light" << (j++);
		LightInfo lightInfo;
		Ogre::Light* ogreLight;
		try {
			ogreLight = _getManager()->createLight(name.str());
		} catch (const std::exception& ex) {
			S_LOG_FAILURE("Could not create light: " << name.str() << "." << ex);
			continue;
		}
		if (ogreLight) {
			ogreLight->setType(Ogre::Light::LT_POINT);
			ogreLight->setDiffuseColour(lightDef.diffuseColour);
			ogreLight->setSpecularColour(lightDef.specularColour);
			ogreLight->setAttenuation(lightDef.range, lightDef.constant, lightDef.linear, lightDef.quadratic);
			ogreLight->setPosition(lightDef.position);
			//ogreLight->setDiffuseColour(Ogre::ColourValue(0.5f,0.0f,0.0f));
			//ogreLight->setSpecularColour(Ogre::ColourValue(0.5f,0.0f,0.0f));
			//ogreLight->setAttenuation(100,1,0,0);
			//ogreLight->setSpotlightRange(Ogre::Degree(60), Ogre::Degree(70));
			//ogreLight->setDirection(Ogre::Vector3::NEGATIVE_UNIT_Y);

			lightInfo.light = ogreLight;
			lightInfo.position = lightDef.position;
			mLights.push_back(lightInfo);
			refreshMovableList();
		}

	}
}

bool Model::hasParticles() const
{
	return !mParticleSystems.empty();
}

const ParticleSystemBindingsPtrSet& Model::getAllParticleSystemBindings() const
{
	return mAllParticleSystemBindings;
}

ParticleSystemSet& Model::getParticleSystems()
{
	return mParticleSystems;
}

LightSet& Model::getLights()
{
	return mLights;
}

bool Model::addSubmodel(SubModel* submodel)
{
	//if the submodel has a skeleton, check if it should be shared with existing models
	if (submodel->getEntity()->getSkeleton()) {
		if (mSkeletonOwnerEntity != 0) {
			submodel->getEntity()->shareSkeletonInstanceWith(mSkeletonOwnerEntity);
		} else {
			mSkeletonOwnerEntity = submodel->getEntity();
			// 			mAnimationStateSet = submodel->getEntity()->getAllAnimationStates();
		}
	}
	auto result = mSubmodels.insert(submodel);
	if (result.second) {
		refreshMovableList();
	}
	return true;
}

bool Model::removeSubmodel(SubModel* submodel)
{
	auto numberRemoved = mSubmodels.erase(submodel);
	if (numberRemoved > 0) {
		refreshMovableList();
	}
	return true;
}

SubModel* Model::getSubModel(size_t index)
{
	size_t i = 0;
	auto result = std::find_if(mSubmodels.begin(), mSubmodels.end(),
		[&i, &index](SubModel*)->bool {return i++ == index;}
	);
	if (result != mSubmodels.end()) {
		return *result;
	}
	S_LOG_FAILURE("Could not find submodel with index "<< index << " in model " << getName());
	return 0;

}

void Model::showPart(const std::string& partName, bool hideOtherParts)
{
	auto I = mModelParts.find(partName);
	if (I != mModelParts.end()) {
		ModelPart& modelPart = I->second;
		if (hideOtherParts) {
			const std::string& groupName = modelPart.getGroupName();
			//make sure that all other parts in the same group are hidden
			PartGroupStore::iterator partBucketI = mGroupsToPartMap.find(groupName);
			if (partBucketI != mGroupsToPartMap.end()) {
				for (auto& part : partBucketI->second) {
					if (part != partName) {
						hidePart(part, true);
					}
				}
			}
		}

		modelPart.show();
	}
}

void Model::hidePart(const std::string& partName, bool dontChangeVisibility)
{
	ModelPartStore::iterator I = mModelParts.find(partName);
	if (I != mModelParts.end()) {
		ModelPart& modelPart = I->second;
		modelPart.hide();
		if (!dontChangeVisibility) {
			modelPart.setVisible(false);
			const std::string& groupName = modelPart.getGroupName();
			//if some part that was hidden before now should be visible
			PartGroupStore::iterator partBucketI = mGroupsToPartMap.find(groupName);
			if (partBucketI != mGroupsToPartMap.end()) {
				for (auto& part : partBucketI->second) {
					if (part != partName) {
						ModelPartStore::iterator I_modelPart = mModelParts.find(partName);
						if (I_modelPart != mModelParts.end()) {
							if (I_modelPart->second.getVisible()) {
								I_modelPart->second.show();
								break;
							}
						}
					}
				}
			}

		}
	}

}

void Model::setVisible(bool visible)
{
	mVisible = visible;
	for (auto& movable : mMovableObjects) {
		movable->setVisible(visible);
	}
}

void Model::setDisplaySkeleton(bool display)
{
	for (auto& submodel : mSubmodels) {
		submodel->getEntity()->setDisplaySkeleton(display);
	}
}

bool Model::getDisplaySkeleton(void) const
{
	const auto I = mSubmodels.begin();
	if (I != mSubmodels.end()) {
		return (*I)->getEntity()->getDisplaySkeleton();
	}
	return false;
}

bool Model::isLoaded() const
{
	//If there's no background loader available the model is loaded in the main thread, and therefore is considered to be loaded already.
	return mBackgroundLoader == nullptr || mBackgroundLoader->getState() == ModelBackgroundLoader::LS_DONE;
}

Ogre::Real Model::getScale() const
{
	return mScale;
}

const Ogre::Quaternion& Model::getRotation() const
{
	return mRotation;
}

ModelDefinition::UseScaleOf Model::getUseScaleOf() const
{
	return mDefinition->getUseScaleOf();
}

Action* Model::getAction(const std::string& name)
{
	const auto I = mActions.find(name);
	if (I == mActions.end()) {
		return nullptr;
	}
	return &(I->second);
}

Action* Model::getAction(const ActivationDefinition::Type type, const std::string& trigger)
{
	for (auto& action : mDefinition->mActions) {
		for (auto& activationDefinition : action->getActivationDefinitions()) {
			if (type == activationDefinition->type && trigger == activationDefinition->trigger) {
				return getAction(action->getName());
			}
		}
	}
	return 0;
}

void Model::resetSubmodels()
{
	if (!mSubmodels.empty()) {
		for (auto& submodel : mSubmodels) {
			delete submodel;
		}
		mSubmodels.clear();
		refreshMovableList();
	}
	mModelParts.clear();
}

void Model::resetParticles()
{
	if (!mParticleSystems.empty()) {
		for (auto& system : mParticleSystems) {
			delete system;
		}
		mParticleSystems.clear();
		refreshMovableList();
	}
	mAllParticleSystemBindings.clear();
}

void Model::resetLights()
{
	if (!mLights.empty()) {
		for (auto& lightInfo : mLights) {
			Ogre::Light* light = lightInfo.light;
			if (light) {
				//Try first with the manager to which the light belongs to. If none is found, try to see if we belong to a maneger. And if that's not true either, just delete it.
				if (light->_getManager()) {
					light->_getManager()->destroyLight(light);
				} else if (_getManager()) {
					_getManager()->destroyLight(light);
				} else {
					delete light;
				}

			}
		}
		mLights.clear();
		refreshMovableList();
	}
}

void Model::refreshMovableList()
{
	mMovableObjects.resize(0);
	for (auto& submodel : mSubmodels) {
		mMovableObjects.push_back(submodel->getEntity());
	}
	for (auto& particleSystem : mParticleSystems) {
		mMovableObjects.push_back(particleSystem->getOgreParticleSystem());
	}
	for (auto& lightInfo : mLights) {
		mMovableObjects.push_back(lightInfo.light);
	}

}


Model::AttachPointWrapper Model::attachObjectToAttachPoint(const Ogre::String &attachPointName, Ogre::MovableObject *pMovable, const Ogre::Vector3 &scale, const Ogre::Quaternion &offsetOrientation, const Ogre::Vector3 &offsetPosition)
{
	for (auto& attachPointDef : mDefinition->mAttachPoints) {
		if (attachPointDef.Name == attachPointName) {
			const std::string& boneName = attachPointDef.BoneName;
			//use the rotation in the attach point def
			Ogre::TagPoint* tagPoint = attachObjectToBone(boneName, pMovable, offsetOrientation * attachPointDef.Rotation, offsetPosition, scale);
			if (!mAttachPoints.get()) {
				mAttachPoints = std::unique_ptr<AttachPointWrapperStore>(new AttachPointWrapperStore());
			}

			AttachPointWrapper wrapper;
			wrapper.TagPoint = tagPoint;
			wrapper.Movable = pMovable;
			wrapper.Definition = attachPointDef;
			mAttachPoints->push_back(wrapper);
			return wrapper;
		}
	}
	return AttachPointWrapper();
}

bool Model::hasAttachPoint(const std::string& attachPoint) const
{
	return std::find_if(mDefinition->mAttachPoints.begin(), mDefinition->mAttachPoints.end(), [&attachPoint](const AttachPointDefinition& def)->bool { return def.Name == attachPoint; } ) != mDefinition->mAttachPoints.end();
}

Ogre::AnimationState* Model::getAnimationState(const Ogre::String& name)
{
	if (!mSubmodels.empty() && mSkeletonOwnerEntity) {
		return mSkeletonOwnerEntity->getAnimationState(name);
	} else {
		return nullptr;
	}
}

Ogre::AnimationStateSet* Model::getAllAnimationStates()
{
	if (!mSubmodels.empty() && mSkeletonOwnerEntity) {
		return mSkeletonOwnerEntity->getAllAnimationStates();
	} else {
		return nullptr;
	}
}

Ogre::SkeletonInstance * Model::getSkeleton() const
{
	if (!mSubmodels.empty() && mSkeletonOwnerEntity) {
		return mSkeletonOwnerEntity->getSkeleton();
	} else {
		return nullptr;
	}
}

Ogre::TagPoint* Model::attachObjectToBone(const Ogre::String &boneName, Ogre::MovableObject *pMovable, const Ogre::Quaternion &offsetOrientation, const Ogre::Vector3 &offsetPosition)
{
	return attachObjectToBone(boneName, pMovable, offsetOrientation, offsetPosition, Ogre::Vector3::UNIT_SCALE);
}

Ogre::TagPoint* Model::attachObjectToBone(const Ogre::String &boneName, Ogre::MovableObject *pMovable, const Ogre::Quaternion &offsetOrientation, const Ogre::Vector3 &offsetPosition, const Ogre::Vector3 &scale)
{
	if (!mSubmodels.empty() && mSkeletonOwnerEntity) {
		Ogre::Entity* entity = mSkeletonOwnerEntity;

		Ogre::TagPoint* tagPoint = entity->attachObjectToBone(boneName, pMovable, offsetOrientation, offsetPosition);

		if (mParentNode) {
			//since we're using inherit scale on the tagpoint, divide by the parent's scale now, so it evens out later on when the TagPoint is scaled in TagPoint::_updateFromParent(
			Ogre::Vector3 parentScale = mParentNode->_getDerivedScale();
			tagPoint->setScale(scale / parentScale);
		} else {
			//no parent node, this is not good...
			tagPoint->setScale(scale);
		}
		return tagPoint;

	} else {
		OGRE_EXCEPT(Ogre::Exception::ERR_ITEM_NOT_FOUND, "There are no entities loaded!", "Model::attachObjectToBone");
	}
}

Ogre::MovableObject* Model::detachObjectFromBone(const Ogre::String &movableName)
{

	if (!mSubmodels.empty() && mSkeletonOwnerEntity) {
		if (mAttachPoints.get()) {
			for (auto I = mAttachPoints->begin(); I != mAttachPoints->end(); ++I) {
				if (I->Movable->getName() == movableName) {
					Ogre::MovableObject* result = I->Movable;
					if (I->TagPoint == I->Movable->getParentNode()) {
						result = mSkeletonOwnerEntity->detachObjectFromBone(movableName);
					}
					mAttachPoints->erase(I);
					return result;
				}
			}
		}
		return nullptr;
	} else {
		OGRE_EXCEPT(Ogre::Exception::ERR_ITEM_NOT_FOUND, "There are no entities loaded!", "Model::detachObjectFromBone");
	}
}

//-----------------------------------------------------------------------
void Model::detachAllObjectsFromBone(void)
{
	if (!mSubmodels.empty() && mSkeletonOwnerEntity) {
		mSkeletonOwnerEntity->detachAllObjectsFromBone();
		mAttachPoints = std::unique_ptr<AttachPointWrapperStore>(nullptr);

	} else {
		OGRE_EXCEPT(Ogre::Exception::ERR_ITEM_NOT_FOUND, "There are no entities loaded!", "Model::detachAllObjectsFromBone");
	}
}

/** Overridden - see MovableObject.
 */
void Model::_notifyCurrentCamera(Ogre::Camera* cam)
{
	MovableObject::_notifyCurrentCamera(cam);
	if (isVisible()) {
		if (mVisible) {

			for (auto& movable : mMovableObjects) {
				movable->_notifyCurrentCamera(cam);
			}

			// Notify any child objects
			for (auto& child : mChildObjectList) {
				child.second->_notifyCurrentCamera(cam);
			}
		}
	}
}

void Model::_notifyMoved()
{
	MovableObject::_notifyMoved();
	for (auto& movable : mMovableObjects) {
		movable->_notifyMoved();
	}
}

// Overridden - see MovableObject.
void Model::setRenderQueueGroup(Ogre::RenderQueueGroupID queueID)
{
	for (auto& movable : mMovableObjects) {
		movable->setRenderQueueGroup(queueID);
	}
}

/** Overridden - see MovableObject.
 */
const Ogre::AxisAlignedBox& Model::getBoundingBox(void) const
{
	mFull_aa_box.setNull();

	for (auto& movable : mMovableObjects) {
		mFull_aa_box.merge(movable->getBoundingBox());
	}

	return mFull_aa_box;
}

/** Overridden - see MovableObject.
 */
const Ogre::AxisAlignedBox& Model::getWorldBoundingBox(bool derive) const
{
	mWorldFull_aa_box.setNull();

	for (auto& movable : mMovableObjects) {
		mWorldFull_aa_box.merge(movable->getWorldBoundingBox(derive));
	}

	return mWorldFull_aa_box;
}

Ogre::Real Model::getBoundingRadius() const
{
	Ogre::Real rad(0);
	for (auto& movable : mMovableObjects) {
		rad = std::max<Ogre::Real>(rad, movable->getBoundingRadius());
	}
	return rad;

}

/** Overridden - see MovableObject.
 */
void Model::_updateRenderQueue(Ogre::RenderQueue* queue)
{
	//check with both the model visibility setting and with the general model setting to see whether the model should be shown
	if (isVisible()) {
		for (auto& submodel : mSubmodels) {
			if (submodel->getEntity()->isVisible()) {
				submodel->getEntity()->_updateRenderQueue(queue);
			}
		}

		if (getSkeleton() != 0) {
			for (auto& child : mChildObjectList) {
				//make sure to do _update here, else attached entities won't be updated if no animation is playing
				child.second->getParentNode()->_update(true, true);
				if (child.second->isVisible())
					child.second->_updateRenderQueue(queue);
			}
		}
		for (auto& particleSystemInfo : mParticleSystems) {
			Ogre::ParticleSystem* particleSystem = particleSystemInfo->getOgreParticleSystem();
			//Alter the direction of the emitter so that it's always emits upwards in the world
			if (!particleSystemInfo->getDirection().isNaN()) {
				Ogre::Vector3 direction = particleSystem->getParentNode()->convertWorldToLocalPosition(particleSystemInfo->getDirection());
				direction.normalise();
				//const Ogre::Quaternion& rotation = particleSystem->getParentNode()->_getDerivedOrientation();
				Ogre::Quaternion rotation = particleSystem->getParentNode()->convertWorldToLocalOrientation(Ogre::Quaternion(Ogre::Degree(0), particleSystemInfo->getDirection()));
				for (int i = 0; i < particleSystem->getNumEmitters(); ++i) {
					particleSystem->getEmitter(i)->setDirection(rotation * particleSystemInfo->getDirection());
				}
			}
			particleSystem->_updateRenderQueue(queue);
		}

		for (auto& lightInfo : mLights) {
			lightInfo.light->_updateRenderQueue(queue);
		}

	}

}

/** Overridden from MovableObject */
const Ogre::String& Model::getMovableType(void) const
{
	return sMovableType;
}

void Model::setRenderingDistance(Ogre::Real dist)
{
	MovableObject::setRenderingDistance(dist);
	for (auto& movable : mMovableObjects) {
		movable->setRenderingDistance(dist);
	}
}

void Model::setQueryFlags(unsigned long flags)
{
	MovableObject::setQueryFlags(flags);
	for (auto& movable : mMovableObjects) {
		movable->setQueryFlags(flags);
	}
}

void Model::addQueryFlags(unsigned long flags)
{
	MovableObject::addQueryFlags(flags);
	for (auto& movable : mMovableObjects) {
		movable->addQueryFlags(flags);
	}
}

void Model::removeQueryFlags(unsigned long flags)
{
	MovableObject::removeQueryFlags(flags);
	for (auto& movable : mMovableObjects) {
		movable->removeQueryFlags(flags);
	}
}

void Model::visitRenderables(Ogre::Renderable::Visitor* visitor, bool debugRenderables)
{
	if (isVisible()) {
		for (auto& movable : mMovableObjects) {
			movable->visitRenderables(visitor, debugRenderables);
		}
	}
}

/** Overridden from MovableObject */
void Model::_notifyAttached(Ogre::Node* parent, bool isTagPoint)
{
	if (parent != mParentNode) {
		MovableObject::_notifyAttached(parent, isTagPoint);
	}

	for (auto& submodel : mSubmodels) {
		if (submodel->getEntity()->getParentNode() != parent) {
			submodel->getEntity()->_notifyAttached(parent, isTagPoint);
		}
	}

	auto I = mParticleSystems.begin();
	while (I != mParticleSystems.end()) {

		(*I)->getOgreParticleSystem()->_notifyAttached(parent, isTagPoint);
		try {
			//Try to trigger a load of any image resources used by affectors.
			//The reason we want to do this now is that otherwise it will happen during rendering. An exception will then be thrown
			//which will bubble all the way up to the main loop, thus aborting all frames.
			(*I)->getOgreParticleSystem()->_update(0);

			//Check if the material used is transparent. If so, assign it a later render queue.
			//This is done to make transparent particle systems play better with the foliage and the water.
			//The foliage would be rendered at an earlier render queue (RENDER_QUEUE_6 normally) and the water at RENDER_QUEUE_8.
			//This of course means that there's still an issue when the camera is below the water
			//(as the water, being rendered first, will prevent the particles from being rendered). That will need to be solved.
			std::pair<Ogre::ResourcePtr, bool> result = Ogre::MaterialManager::getSingleton().createOrRetrieve((*I)->getOgreParticleSystem()->getMaterialName(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
			Ogre::MaterialPtr materialPtr = result.first.staticCast<Ogre::Material>();
			if (!materialPtr.isNull()) {
				if (materialPtr->isTransparent()) {
					(*I)->getOgreParticleSystem()->setRenderQueueGroup(Ogre::RENDER_QUEUE_9);
				}
			}

			++I;
		} catch (const Ogre::Exception& ex) {
			//An exception occurred when forcing an update of the particle system. Remove it.
			S_LOG_FAILURE("Error when loading particle system " << (*I)->getOgreParticleSystem()->getName() << ". Removing it.");
			mMovableObjects.erase(std::find(std::begin(mMovableObjects), std::end(mMovableObjects), (*I)->getOgreParticleSystem()));
			delete *I;
			I = mParticleSystems.erase(I);
		}
	}

	for (auto& lightInfo : mLights) {
		lightInfo.light->_notifyAttached(parent, isTagPoint);
	}

}

bool Model::isVisible(void) const
{
	//check with both the model visibility setting and with the general model setting to see whether the model should be shown
	return Ogre::MovableObject::isVisible() && ModelDefinitionManager::getSingleton().getShowModels();
}

Model* Model::createModel(Ogre::SceneManager& sceneManager, const std::string& modelType, const std::string& name)
{

	Ogre::String modelName(name);
	if (name == "") {
		std::stringstream ss;
		ss << "__AutogenModel_" << Model::msAutoGenId++;
		modelName = ss.str();
	}

	// delegate to factory implementation
	Ogre::NameValuePairList params;
	params["modeldefinition"] = modelType;
	return static_cast<Model*> (sceneManager.createMovableObject(modelName, ModelFactory::FACTORY_TYPE_NAME, &params));

}

const Model::AttachPointWrapperStore* Model::getAttachedPoints() const
{
	return mAttachPoints.get();
}

Ogre::String ModelFactory::FACTORY_TYPE_NAME = "Model";
//-----------------------------------------------------------------------
ModelFactory::ModelFactory(Eris::EventService& eventService) : mEventService(eventService)
{

}

const Ogre::String& ModelFactory::getType(void) const
{
	return FACTORY_TYPE_NAME;
}
//-----------------------------------------------------------------------
Ogre::MovableObject* ModelFactory::createInstanceImpl(const Ogre::String& name, const Ogre::NameValuePairList* params)
{

	// must have mesh parameter
	if (params != 0) {
		auto ni = params->find("modeldefinition");
		if (ni != params->end()) {
			Model* model = new Model(name);
			model->create(ni->second, mEventService);
			return model;
		}

	}
	OGRE_EXCEPT(Ogre::Exception::ERR_INVALIDPARAMS,
			"'modeldefinition' parameter required when constructing a Model.",
			"ModelFactory::createInstance");

}
//-----------------------------------------------------------------------
void ModelFactory::destroyInstance(Ogre::MovableObject* obj)
{
	delete obj;
}

}
}
}
