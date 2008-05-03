/***************************************************************************
 *   Copyright (C) 2005-2008 by the FIFE team                              *
 *   http://www.fifengine.de                                               *
 *   This file is part of FIFE.                                            *
 *                                                                         *
 *   FIFE is free software; you can redistribute it and/or modify          *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA              *
 ***************************************************************************/

// Standard C++ library includes
#include <iostream>

// 3rd party library includes
#include <SDL.h>

// FIFE includes
// These includes are split up in two parts, separated by one empty line
// First block: files included from the FIFE root src directory
// Second block: files included from the same folder
#include "util/log/logger.h"
#include "util/base/exception.h"
#include "util/math/fife_math.h"
#include "model/metamodel/grids/cellgrid.h"
#include "model/metamodel/abstractpather.h"
#include "model/metamodel/action.h"
#include "model/metamodel/timeprovider.h"
#include "model/structures/layer.h"
#include "model/structures/map.h"
#include "model/structures/instancetree.h"

#include "instance.h"

namespace FIFE {
	static Logger _log(LM_INSTANCE);
	
	class ActionInfo {
	public:
		ActionInfo(AbstractPather* pather, const Location& curloc): 
			m_action(NULL), 
			m_target(NULL), 
			m_speed(0), 
			m_repeating(false),
			m_action_start_time(SDL_GetTicks()),
			m_prev_call_time(m_action_start_time),
			m_pather_session_id(-1),
			m_pather(pather),
			m_leader(NULL) {}

		~ActionInfo() {
			if (m_pather_session_id != -1) {
				m_pather->cancelSession(m_pather_session_id);
			}
			delete m_target;
			m_target = NULL;
		}

		// Current action, owned by object
		Action* m_action;
		// target location for ongoing movement
		Location* m_target;
		// current movement speed
		double m_speed;
		// should action be repeated? used only for non-moving actions, moving ones repeat until movement is finished
		bool m_repeating;
		// action start time (ticks)
		unsigned int m_action_start_time;
		// ticks since last call
		unsigned int m_prev_call_time;
		// current time for action processing (set by Instance::update), optimized to avoid multiple calls to GetTicks
		unsigned int m_cur_time;
		// session id for pather
		int m_pather_session_id;
		// pather
		AbstractPather* m_pather;
		// leader for follow activity
		Instance* m_leader;
	};
	
	class SayInfo {
	public:
		SayInfo(const std::string& txt, unsigned int duration):
			m_txt(txt),
			m_duration(duration),
			m_start_time(SDL_GetTicks()) {
		}
		std::string m_txt;
		unsigned int m_duration;
		unsigned int m_start_time;
	};	
	
	Instance::InstanceActivity::InstanceActivity(Instance& source):
		m_location(source.m_location),
		m_facinglocation(),
		m_action(),
		m_speed(0),
		m_timemultiplier(1.0),
		m_saytxt(""),
 		m_changelisteners(),
		m_actionlisteners(),
		m_actioninfo(NULL),
		m_sayinfo(NULL),
		m_timeprovider(NULL) {
		if (source.m_facinglocation) {
			m_facinglocation = *source.m_facinglocation;
		}
	}
	
	Instance::InstanceActivity::~InstanceActivity() {
		delete m_actioninfo;
		delete m_sayinfo;
		delete m_timeprovider;
	}
	
	void Instance::InstanceActivity::update(Instance& source) {
		source.m_changeinfo = ICHANGE_NO_CHANGES;
		if (m_location != source.m_location) {
			source.m_changeinfo |= ICHANGE_LOC;
			m_location = source.m_location;
		}
		if (source.m_facinglocation && (m_facinglocation != *source.m_facinglocation)) {
			source.m_changeinfo |= ICHANGE_FACING_LOC;
			m_facinglocation = *source.m_facinglocation;
		}
		if (m_actioninfo && (m_speed != m_actioninfo->m_speed)) {
			source.m_changeinfo |= ICHANGE_SPEED;
			m_speed = m_actioninfo->m_speed;
		}
		if (m_actioninfo && (m_action != m_actioninfo->m_action)) {
			source.m_changeinfo |= ICHANGE_ACTION;
			m_action = m_actioninfo->m_action;
		}
		if (m_timeprovider && (m_timemultiplier != m_timeprovider->getMultiplier())) {
			source.m_changeinfo |= ICHANGE_TIME_MULTIPLIER;
			m_timemultiplier = m_timeprovider->getMultiplier();
		}
		if (m_sayinfo && (m_saytxt != m_sayinfo->m_txt)) {
			source.m_changeinfo |= ICHANGE_SAYTEXT;
			m_saytxt = m_sayinfo->m_txt;
		}
		
		if (source.m_changeinfo != ICHANGE_NO_CHANGES) {
			std::vector<InstanceChangeListener*>::iterator i = m_changelisteners.begin();
			while (i != m_changelisteners.end()) {
				(*i)->onInstanceChanged(&source, source.m_changeinfo);
				++i;
			}
		}
	}

	Instance::Instance(Object* object, const Location& location, const std::string& identifier):
		m_id(identifier),
		m_rotation(0),
		m_activity(NULL),
		m_changeinfo(ICHANGE_NO_CHANGES),
		m_object(object),
		m_location(location),
		m_facinglocation(NULL),
		m_visual(NULL) {
	}

	Instance::~Instance() {
		delete m_activity;
		delete m_facinglocation;
		delete m_visual;
	}
	
	void Instance::initializeChanges() {
		if (!m_activity) {
			m_activity = new InstanceActivity(*this);
		}
	}
	
	void Instance::setLocation(const Location& loc) {
		initializeChanges();
		m_location = loc;
		bindTimeProvider();
	}

	void Instance::setRotation(int rotation) {
		m_rotation = rotation;
		m_changeinfo |= ICHANGE_ROTATION;
	}

	void Instance::addActionListener(InstanceActionListener* listener) {
		initializeChanges();
		m_activity->m_actionlisteners.push_back(listener);
	}

	void Instance::removeActionListener(InstanceActionListener* listener) {
		if (!m_activity) {
			return;
		}
		std::vector<InstanceActionListener*>::iterator i = m_activity->m_actionlisteners.begin();
		while (i != m_activity->m_actionlisteners.end()) {
			if ((*i) == listener) {
				m_activity->m_actionlisteners.erase(i);
				return;
			}
			++i;
		}
		FL_WARN(_log, "Cannot remove unknown listener");
	}

	void Instance::addChangeListener(InstanceChangeListener* listener) {
		initializeChanges();
		m_activity->m_changelisteners.push_back(listener);
	}

	void Instance::removeChangeListener(InstanceChangeListener* listener) {
		if (!m_activity) {
			return;
		}
		std::vector<InstanceChangeListener*>::iterator i = m_activity->m_changelisteners.begin();
		while (i != m_activity->m_changelisteners.end()) {
			if ((*i) == listener) {
				m_activity->m_changelisteners.erase(i);
				return;
			}
			++i;
		}
		FL_WARN(_log, "Cannot remove unknown listener");
	}

	void Instance::initalizeAction(const std::string& action_name) {
		assert(m_object);
		assert(m_activity);
		if (m_activity->m_actioninfo) {
			delete m_activity->m_actioninfo;
			m_activity->m_actioninfo = NULL;
		}
		m_activity->m_actioninfo = new ActionInfo(m_object->getPather(), m_location);
		m_activity->m_actioninfo->m_action = m_object->getAction(action_name);
		if (!m_activity->m_actioninfo->m_action) {
			delete m_activity->m_actioninfo;
			m_activity->m_actioninfo = NULL;
			throw NotFound(std::string("action ") + action_name + " not found");
		}
	}

	void Instance::move(const std::string& action_name, const Location& target, const double speed) {
		initializeChanges();
		initalizeAction(action_name);
		m_activity->m_actioninfo->m_target = new Location(target);
		m_activity->m_actioninfo->m_speed = speed;
		setFacingLocation(target);
		FL_DBG(_log, LMsg("starting action ") <<  action_name << " from" << m_location << " to " << target << " with speed " << speed);
	}
	
	void Instance::follow(const std::string& action_name, Instance* leader, const double speed) {
		initializeChanges();
		initalizeAction(action_name);
		m_activity->m_actioninfo->m_target = new Location(leader->getLocationRef());
		m_activity->m_actioninfo->m_speed = speed;
		m_activity->m_actioninfo->m_leader = leader;
		setFacingLocation(*m_activity->m_actioninfo->m_target);
		FL_DBG(_log, LMsg("starting action ") <<  action_name << " from" << m_location << " to " << *m_activity->m_actioninfo->m_target << " with speed " << speed);
	}

	void Instance::act(const std::string& action_name, const Location& direction, bool repeating) {
		initializeChanges();
		initalizeAction(action_name);
		m_activity->m_actioninfo->m_repeating = repeating;
		setFacingLocation(direction);
	}

	void Instance::say(const std::string& text, unsigned int duration) {
		initializeChanges();
		delete m_activity->m_sayinfo;
		m_activity->m_sayinfo = NULL;
		
		if (text != "") {
			m_activity->m_sayinfo = new SayInfo(text, duration);
		}
	}

	const std::string* Instance::getSayText() const {
		if (m_activity && m_activity->m_sayinfo) {
			return &m_activity->m_sayinfo->m_txt;
		}
		return NULL;
	}

	void Instance::setFacingLocation(const Location& loc) {
		if (!m_facinglocation) {
			m_facinglocation = new Location(loc);
		} else {
			*m_facinglocation = loc;
		}
	}

	bool Instance::process_movement() {
		FL_DBG(_log, "Moving...");
		ActionInfo* info = m_activity->m_actioninfo;
		// timeslice for this movement
		unsigned int timedelta = scaleTime(getTotalTimeMultiplier(), info->m_cur_time - info->m_prev_call_time);
		FL_DBG(_log, LMsg("timedelta ") <<  timedelta << " prevcalltime " << info->m_prev_call_time);
		// how far we can travel
		double distance_to_travel = (static_cast<double>(timedelta) / 1000.0) * info->m_speed;
		FL_DBG(_log, LMsg("dist ") <<  distance_to_travel);
				
		Location nextLocation = m_location;
		info->m_pather_session_id = info->m_pather->getNextLocation(
			this, *info->m_target,
			distance_to_travel, nextLocation, *m_facinglocation,
			info->m_pather_session_id);
		m_location.getLayer()->getInstanceTree()->removeInstance(this);
		m_location = nextLocation;
		//ExactModelCoordinate a = nextLocation.getMapCoordinates();
		//ExactModelCoordinate b = m_actioninfo->m_target->getMapCoordinates();
		m_location.getLayer()->getInstanceTree()->addInstance(this);
		// return if we are close enough to target to stop
		if (info->m_pather_session_id == -1) {
			return true;
		} 
		return false;
	}

	InstanceChangeInfo Instance::update(unsigned int curticks) {
		if (!m_activity) {
			return ICHANGE_NO_CHANGES;
		}
		m_activity->update(*this);
		if (!m_activity->m_timeprovider) {
			bindTimeProvider();
		}
		
		if (curticks == 0) {
			curticks = SDL_GetTicks();
		}
		ActionInfo* info = m_activity->m_actioninfo;
		if (info) {
			info->m_cur_time = curticks;
			FL_DBG(_log, LMsg("updating instance, ticks = ") << curticks);

			if (info->m_target) {
				FL_DBG(_log, "action contains target for movement");
				// update target if needed
				if (info->m_leader && (info->m_leader->getLocationRef() != *info->m_target)) {
					*info->m_target = info->m_leader->getLocation();
				}
				bool movement_finished = process_movement();
				if (movement_finished) {
					FL_DBG(_log, "movement finished");
					finalizeAction();
				}
			} else {
				FL_DBG(_log, "action does not contain target for movement");
				if (scaleTime(getTotalTimeMultiplier(), curticks - info->m_action_start_time) >= info->m_action->getDuration()) {
					if (info->m_repeating) {
						info->m_action_start_time = curticks;
					} else {
						finalizeAction();
					}
				}
			}

			m_activity->m_actioninfo->m_prev_call_time = curticks;
		}
		if (m_activity->m_sayinfo) {
			if (m_activity->m_sayinfo->m_duration > 0) {
				if (scaleTime(getTotalTimeMultiplier(), curticks - m_activity->m_sayinfo->m_start_time) > m_activity->m_sayinfo->m_duration) {
					say("");
				}
			}
		}
		return m_changeinfo;
	}

	void Instance::finalizeAction() {
		FL_DBG(_log, "finalizing action");
		assert(m_activity);
		assert(m_activity->m_actioninfo);

		Action* action = m_activity->m_actioninfo->m_action;
		delete m_activity->m_actioninfo;
		m_activity->m_actioninfo = NULL;

		std::vector<InstanceActionListener*>::iterator i = m_activity->m_actionlisteners.begin();
		while (i != m_activity->m_actionlisteners.end()) {
			(*i)->onInstanceActionFinished(this, action);
			++i;
		}
	}

	Action* Instance::getCurrentAction() const {
		if (m_activity && m_activity->m_actioninfo) {
			return m_activity->m_actioninfo->m_action;
		}
		return NULL;
	}

	Location Instance::getTargetLocation() const {
		if (m_activity && m_activity->m_actioninfo && m_activity->m_actioninfo->m_target) {
			return *m_activity->m_actioninfo->m_target;
		}
		return m_location;
	}

	double Instance::getMovementSpeed() const {
		if (m_activity && m_activity->m_actioninfo) {
			return m_activity->m_actioninfo->m_speed;
		}
		return 0;
	}

	Location Instance::getFacingLocation() {
		return this->getFacingLocationRef();
	}

	Location& Instance::getFacingLocationRef() {
		if (!m_facinglocation) {
			m_facinglocation = new Location(m_location);
			m_facinglocation->setExactLayerCoordinates(m_facinglocation->getExactLayerCoordinates() + ExactModelCoordinate(1.0, 0.0));
			//m_facinglocation->setLayerCoordinates(ModelCoordinate(1,0));
		}
		return *m_facinglocation;
	}

	int Instance::getActionRuntime() const {
		if (m_activity && m_activity->m_actioninfo) {
			return SDL_GetTicks() - m_activity->m_actioninfo->m_action_start_time;
		}
		return -1;
	}
	
	void Instance::bindTimeProvider() {
		float multiplier = 1.0;
		if (m_activity->m_timeprovider) {
			multiplier = m_activity->m_timeprovider->getMultiplier();
		}
		delete m_activity->m_timeprovider;
		m_activity->m_timeprovider = NULL;
		
		if (m_location.getLayer()) {
			Map* map = m_location.getLayer()->getMap();
			if (map) {
				m_activity->m_timeprovider = new TimeProvider(map->getTimeProvider());
			}
		}
		if (!m_activity->m_timeprovider) {
			m_activity->m_timeprovider = new TimeProvider(NULL);
		}
		m_activity->m_timeprovider->setMultiplier(multiplier);
	}
	
	void Instance::refresh() {
		initializeChanges();
		bindTimeProvider();
	}
	
	void Instance::setTimeMultiplier(float multip) {
		initializeChanges();
		if (!m_activity->m_timeprovider) {
			bindTimeProvider();
		}
		m_activity->m_timeprovider->setMultiplier(multip);
	}
	
	float Instance::getTimeMultiplier() {
		if (m_activity && m_activity->m_timeprovider) {
			return m_activity->m_timeprovider->getMultiplier();
		}
		return 1.0;
	}
	
	float Instance::getTotalTimeMultiplier() {
		if (m_activity && m_activity->m_timeprovider) {
			return m_activity->m_timeprovider->getTotalMultiplier();
		}
		return 1.0;
	}
}

