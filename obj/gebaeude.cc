/*
 * This file is part of the Simutrans-Extended project under the Artistic License.
 * (see LICENSE.txt)
 */

#include <string.h>
#include <ctype.h>
#include <algorithm>

#ifdef MULTI_THREAD
#include "../utils/simthread.h"
static pthread_mutex_t sync_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t add_to_city_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

#include "../bauer/hausbauer.h"
#include "../bauer/wegbauer.h"
#include "../bauer/tunnelbauer.h"
#include "../bauer/brueckenbauer.h"
#include "../simworld.h"
#include "simobj.h"
#include "../simfab.h"
#include "../simhalt.h"
#include "../gui/simwin.h"
#include "../simcity.h"
#include "../player/simplay.h"
#include "../simdebug.h"
#include "../simintr.h"
#include "../simsignalbox.h"


#include "../descriptor/building_desc.h"
#include "../descriptor/intro_dates.h"

#include "../descriptor/ground_desc.h"

#include "../utils/cbuffer_t.h"
#include "../utils/simrandom.h"

#include "../dataobj/loadsave.h"
#include "../dataobj/translator.h"
#include "../dataobj/settings.h"
#include "../dataobj/environment.h"

#include "../gui/building_info.h"
#include "../gui/headquarter_info.h"
#include "../gui/obj_info.h"

#include "gebaeude.h"


/**
 * Initializes all variables with safe, usable values
 */
void gebaeude_t::init()
{
	tile = NULL;
	anim_time = 0;
	sync = false;
	show_construction = false;
	remove_ground = true;
	is_factory = false;
	season = 0;
	background_animated = false;
	remove_ground = true;
	anim_frame = 0;
	//	construction_start = 0; // init in set_tile()
	ptr.fab = NULL;
	passengers_generated_commuting = 0;
	passengers_succeeded_commuting = 0;
	passenger_success_percent_last_year_commuting = 65535;
	passengers_generated_visiting = 0;
	passengers_succeeded_visiting = 0;
	passenger_success_percent_last_year_visiting = 65535;
	available_jobs_by_time = std::numeric_limits<sint64>::min();
	mail_generated = 0;
	mail_delivery_succeeded_last_year = 65535;
	mail_delivery_succeeded = 0;
	mail_delivery_success_percent_last_year = 65535;
	is_in_world_list = 0;
	loaded_passenger_and_mail_figres = false;
}


gebaeude_t::gebaeude_t(obj_t::typ type) :
#ifdef INLINE_OBJ_TYPE
	obj_t(type)
#else
	obj_t()
#endif
{
	init();
}


gebaeude_t::gebaeude_t(loadsave_t *file, bool do_not_add_to_world_list) :
#ifdef INLINE_OBJ_TYPE
	obj_t(obj_t::gebaeude)
#else
	obj_t()
#endif
{
	init();
	if (do_not_add_to_world_list)
	{
		is_in_world_list = -1;
	}
	rdwr(file);
	if(file->is_version_less(88, 2)) {
		set_yoff(0);
	}
	if (tile  &&  tile->get_phases()>1) {
		welt->sync_eyecandy.add(this);
		sync = true;
	}
}



#ifdef INLINE_OBJ_TYPE
gebaeude_t::gebaeude_t(obj_t::typ type, koord3d pos, player_t *player, const building_tile_desc_t *t) :
	obj_t(type, pos)
{
	init(player, t);
}

gebaeude_t::gebaeude_t(koord3d pos, player_t *player, const building_tile_desc_t *t) :
	obj_t(obj_t::gebaeude, pos)
{
	init(player, t);
}

void gebaeude_t::init(player_t *player, const building_tile_desc_t *t)
#else
gebaeude_t::gebaeude_t(koord3d pos, player_t *player, const building_tile_desc_t *t) :
	obj_t(pos)
#endif
{
	set_owner(player);

	init();
	if(t) {
		set_tile(t,true); // this will set init time etc.
		sint64 maint;
		if (tile->get_desc()->get_base_maintenance() == PRICE_MAGIC)
		{
			maint = welt->get_settings().maint_building*tile->get_desc()->get_level();
		}
		else
		{
			maint = tile->get_desc()->get_maintenance();
		}
		player_t::add_maintenance(get_owner(), maint, tile->get_desc()->get_finance_waytype());
	}

	const building_desc_t::btype type = tile->get_desc()->get_type();

	if (type == building_desc_t::city_res)
	{
		const uint16 population = tile->get_desc()->get_population_and_visitor_demand_capacity();
		people.population = population == 65535 ? tile->get_desc()->get_level() * welt->get_settings().get_population_per_level() : population;
		adjusted_people.population = welt->calc_adjusted_monthly_figure(people.population);
		if (people.population > 0 && adjusted_people.population == 0)
		{
			adjusted_people.population = 1;
		}
	}
	else if (type == building_desc_t::city_ind)
	{
		people.visitor_demand = adjusted_people.visitor_demand = 0;
	}
	else if (tile->get_desc()->is_factory() && tile->get_desc()->get_population_and_visitor_demand_capacity() == 65535)
	{
		adjusted_people.visitor_demand = 65535;
	}
	else
	{
		const uint16 population_and_visitor_demand_capacity = tile->get_desc()->get_population_and_visitor_demand_capacity();
		people.visitor_demand = population_and_visitor_demand_capacity == 65535 ? tile->get_desc()->get_level() * welt->get_settings().get_visitor_demand_per_level() : population_and_visitor_demand_capacity;
		adjusted_people.visitor_demand = welt->calc_adjusted_monthly_figure(people.visitor_demand);
		if (people.visitor_demand > 0 && adjusted_people.visitor_demand == 0)
		{
			adjusted_people.visitor_demand = 1;
		}
	}

	jobs = tile->get_desc()->get_employment_capacity() == 65535 ? (is_monument() || type == building_desc_t::city_res) ? 0 : tile->get_desc()->get_level() * welt->get_settings().get_jobs_per_level() : tile->get_desc()->get_employment_capacity();
	mail_demand = tile->get_desc()->get_mail_demand_and_production_capacity() == 65535 ? is_monument() ? 0 : tile->get_desc()->get_level() * welt->get_settings().get_mail_per_level() : tile->get_desc()->get_mail_demand_and_production_capacity();

	adjusted_jobs = welt->calc_adjusted_monthly_figure(jobs);
	if (jobs > 0 && adjusted_jobs == 0)
	{
		adjusted_jobs = 1;
	}

	adjusted_mail_demand = welt->calc_adjusted_monthly_figure(mail_demand);
	if (mail_demand > 0 && adjusted_mail_demand == 0)
	{
		adjusted_mail_demand = 1;
	}

	// get correct y offset for bridges
	grund_t *gr = welt->lookup(get_pos());
	if (gr  &&  gr->get_weg_hang() != gr->get_grund_hang()) {
		set_yoff(-gr->get_weg_yoff());
	}
	check_road_tiles(false);

	// This sets the number of jobs per building at initialisation to zero. As time passes,
	// more jobs become available. This is necessary because, if buildings are initialised
	// with their maximum number of jobs, there will be too many jobs available by a factor
	// of two. This is because, for any given time period in which the total population is
	// equal to the total number of jobs available, X people will arrive and X job slots
	// will be created. The sum total of this should be zero, but if buildings start with
	// their maximum number of jobs, this ends up being the base line number, effectively
	// doubling the number of available jobs.
	available_jobs_by_time = welt->get_ticks();
}

stadt_t* gebaeude_t::get_stadt() const
{
	return ptr.fab != NULL ? is_factory ? ptr.fab->get_city() : ptr.stadt : NULL;
}

/**
 * Destructor. Removes this from the list of sync objects if necessary.
 */
gebaeude_t::~gebaeude_t()
{
	if (welt->is_destroying())
	{
		return;
		// avoid book-keeping
	}

	if (tile->get_desc()->is_signalbox())
	{
		display_coverage_radius(false);
	}

	stadt_t* our_city = get_stadt();
	const bool has_city_defined = our_city != NULL;
	if (!our_city /* && tile->get_desc()->get_type() == building_desc_t::townhall*/)
	{
		const planquadrat_t* tile = welt->access(get_pos().get_2d());
		our_city = tile ? tile->get_city() : NULL;
	}
	if (!our_city)
	{
		const planquadrat_t* tile = welt->access(get_first_tile()->get_pos().get_2d());
		our_city = tile ? tile->get_city() : NULL;
	}
	if (our_city)
	{
		our_city->remove_gebaeude_from_stadt(this, !has_city_defined, false);
	}
	else if(is_in_world_list > 0)
	{
		welt->remove_building_from_world_list(this);
	}


	if (sync) {
		sync = false;
		welt->sync_eyecandy.remove(this);
	}


	// tiles might be invalid, if no description is found during loading
	if (tile && tile->get_desc())
	{
		check_road_tiles(true);
		if (tile->get_desc()->is_attraction())
		{
			welt->remove_attraction(this);
		}
	}

	if (tile)
	{
		sint64 maint;
		if (tile->get_desc()->get_base_maintenance() == PRICE_MAGIC)
		{
			maint = welt->get_settings().maint_building * tile->get_desc()->get_level();
		}
		else
		{
			maint = tile->get_desc()->get_maintenance();
		}
		player_t::add_maintenance(get_owner(), -maint);
	}

	const weighted_vector_tpl<stadt_t*>& staedte = welt->get_cities();
	for (weighted_vector_tpl<stadt_t*>::const_iterator j = staedte.begin(), end = staedte.end(); j != end; ++j)
	{
		(*j)->remove_connected_attraction(this);
	}
}


void gebaeude_t::check_road_tiles(bool del)
{
	const building_desc_t *bdsc = tile->get_desc();
	const koord3d pos = get_pos() - koord3d(tile->get_offset(), 0);
	koord size = bdsc->get_size(tile->get_layout());
	koord k;
	grund_t* gr_this;

	vector_tpl<gebaeude_t*> building_list;
	building_list.append(this);

	for (k.y = 0; k.y < size.y; k.y++)
	{
		for (k.x = 0; k.x < size.x; k.x++)
		{
			koord3d k_3d = koord3d(k, 0) + pos;
			grund_t *gr = welt->lookup(k_3d);
			if (gr)
			{
				gebaeude_t *gb_part = gr->find<gebaeude_t>();
				// there may be buildings with holes
				if (gb_part && gb_part->get_tile()->get_desc() == bdsc)
				{
					building_list.append_unique(gb_part);
				}
			}
		}
	}

	FOR(vector_tpl<gebaeude_t*>, gb, building_list)
	{
		for (uint8 i = 0; i < 8; i++)
		{
			/* This is tricky: roads can change height, and we're currently
			* not keeping track of when they do. We might show
			* up as connecting to a road that's no longer at the right
			* height. Therefore, iterate over all possible road levels when
			* removing, but not when adding new connections. */
			koord pos_neighbour = gb->get_pos().get_2d() + (gb->get_pos().get_2d().neighbours[i]);
			if (del)
			{
				const planquadrat_t *plan = welt->access(pos_neighbour);
				if (!plan)
				{
					continue;
				}
				for (uint32 j = 0; j<plan->get_boden_count(); j++)
				{
					grund_t *bd = plan->get_boden_bei(j);
					weg_t *way = bd->get_weg(road_wt);

					if (way)
					{
						way->connected_buildings.remove(this);
					}
				}
			}
			else
			{
				koord3d pos3d(pos_neighbour, gb->get_pos().z);

				// Check for connected roads. Only roads in immediately neighbouring tiles
				// and only those on the same height will register a connexion.
				gr_this = welt->lookup(pos3d);

				if (!gr_this)
				{
					continue;
				}
				weg_t* const way = gr_this->get_weg(road_wt);
				if (way)
				{
					way->connected_buildings.append_unique(this);
				}
			}
		}
	}
}

void gebaeude_t::rotate90()
{
	obj_t::rotate90();

	// must or can rotate?
	const building_desc_t* const building_desc = tile->get_desc();
	if (building_desc->get_all_layouts() > 1 || building_desc->get_x() * building_desc->get_y() > 1) {
		uint8 layout = tile->get_layout();
		koord new_offset = tile->get_offset();

		if (building_desc->get_type() == building_desc_t::unknown || building_desc->get_all_layouts() <= 4) {
			layout = (layout & 4) + ((layout + 3) % building_desc->get_all_layouts() & 3);
		}
		else {
			static uint8 layout_rotate[16] = { 1, 8, 5, 10, 3, 12, 7, 14, 9, 0, 13, 2, 11, 4, 15, 6 };
			layout = layout_rotate[layout] % building_desc->get_all_layouts();
		}
		// have to rotate the tiles :(
		if (!building_desc->can_rotate() && building_desc->get_all_layouts() == 1) {
			if ((welt->get_settings().get_rotation() & 1) == 0) {
				// rotate 180 degree
				new_offset = koord(building_desc->get_x() - 1 - new_offset.x, building_desc->get_y() - 1 - new_offset.y);
			}
			// do nothing here, since we cannot fix it properly
		}
		else {
			// rotate on ...
			new_offset = koord(building_desc->get_y(tile->get_layout()) - 1 - new_offset.y, new_offset.x);
		}

		// such a tile exist?
		if (building_desc->get_x(layout) > new_offset.x  &&  building_desc->get_y(layout) > new_offset.y) {
			const building_tile_desc_t* const new_tile = building_desc->get_tile(layout, new_offset.x, new_offset.y);
			// add new tile: but make them old (no construction)
			sint64 old_construction_start = construction_start;
			set_tile(new_tile, false);
			construction_start = old_construction_start;
			if (building_desc->get_type() != building_desc_t::dock && !tile->has_image()) {
				// may have a rotation, that is not recoverable
				if (!is_factory  &&  new_offset != koord(0, 0)) {
					welt->set_nosave_warning();
				}
				if (is_factory) {
					// there are factories with a broken tile
					// => this map rotation cannot be reloaded!
					welt->set_nosave();
				}
			}
		}
		else {
			welt->set_nosave();
		}
	}
}


/** sets the corresponding pointer to a factory
 */
void gebaeude_t::set_fab(fabrik_t *fd)
{
	// sets the pointer in non-zero
	if (fd) {
		if (!is_factory  &&  ptr.stadt != NULL) {
			dbg->fatal("gebaeude_t::set_fab()", "building already bound to city!");
		}
		is_factory = true;
		ptr.fab = fd;
		if (adjusted_people.visitor_demand == 65535)
		{
			// We cannot set this until we know what sort of factory that this is.
			// If it is not an end consumer, do not allow any visitor demand by default.
			if (fd->get_sector() == fabrik_t::end_consumer)
			{
				people.visitor_demand = tile->get_desc()->get_level() * welt->get_settings().get_visitor_demand_per_level();
				adjusted_people.visitor_demand = welt->calc_adjusted_monthly_figure(people.visitor_demand);
			}
			else
			{
				adjusted_people.visitor_demand = people.visitor_demand = 0;
			}
		}
	}
	else if (is_factory) {
		ptr.fab = NULL;
	}
}


/** sets the corresponding city
 */
void gebaeude_t::set_stadt(stadt_t *s)
{
	if (is_factory && ptr.fab != NULL)
	{
		if (s == NULL)
		{
			return;
		}
		dbg->fatal("gebaeude_t::set_stadt()", "building at (%s) already bound to factory!", get_pos().get_str());
	}
	// sets the pointer in non-zero
	is_factory = false;
	ptr.stadt = s;
}


/* make this building without construction */
void gebaeude_t::add_alter(sint64 a)
{
	construction_start -= min(a, construction_start);
}


void gebaeude_t::set_tile(const building_tile_desc_t *new_tile, bool start_with_construction)
{
	construction_start = welt->get_ticks();
	purchase_time = welt->get_current_month();

	if (!show_construction  &&  tile != NULL) {
		// mark old tile dirty
		mark_images_dirty();
	}

	show_construction = !new_tile->get_desc()->no_construction_pit() && start_with_construction;
	if (sync) {
		if (new_tile->get_phases() <= 1 && !show_construction) {
			// need to stop animation
#ifdef MULTI_THREAD
			pthread_mutex_lock(&sync_mutex);
#endif
			welt->sync_eyecandy.remove(this);
			sync = false;
			anim_frame = 0;
#ifdef MULTI_THREAD
			pthread_mutex_unlock(&sync_mutex);
#endif
		}
	}
	else if ((new_tile->get_phases()>1 && (!is_factory || get_fabrik()->is_currently_producing())) || show_construction) {
		// needs now animation
#ifdef MULTI_THREAD
		pthread_mutex_lock(&sync_mutex);
#endif
		anim_frame = sim_async_rand(new_tile->get_phases());
		anim_time = 0;
		welt->sync_eyecandy.add(this);
		sync = true;
#ifdef MULTI_THREAD
		pthread_mutex_unlock(&sync_mutex);
#endif
	}
	tile = new_tile;
	remove_ground = tile->has_image() && !tile->get_desc()->needs_ground();
	set_flag(obj_t::dirty);
}


sync_result gebaeude_t::sync_step(uint32 delta_t)
{
	if (construction_start > welt->get_ticks())
	{
		// There were some integer overflow issues with
		// this when some intermediate values were uint32.
		construction_start = welt->get_ticks() - 5000ll;
	}
	if (show_construction) {
		// still under construction?
		if (welt->get_ticks() - construction_start > 5000) {
			set_flag(obj_t::dirty);
			mark_image_dirty(get_image(), 0);
			show_construction = false;
			if (tile->get_phases() <= 1) {
				sync = false;
				return SYNC_REMOVE;
			}
		}
	}
	else {
		if (!is_factory || get_fabrik()->is_currently_producing()) {
			// normal animated building
			anim_time += delta_t;
			if (anim_time > tile->get_desc()->get_animation_time()) {
				anim_time -= tile->get_desc()->get_animation_time();

				// old positions need redraw
				if (background_animated) {
					set_flag(obj_t::dirty);
					mark_images_dirty();
				}
				else {
					// try foreground
					image_id image = tile->get_foreground(anim_frame, season);
					mark_image_dirty(image, 0);
				}

				anim_frame++;
				if (anim_frame >= tile->get_phases()) {
					anim_frame = 0;
				}

				if (!background_animated) {
					// next phase must be marked dirty too ...
					image_id image = tile->get_foreground(anim_frame, season);
					mark_image_dirty(image, 0);
				}
			}
		}
	}
	return SYNC_OK;
}



void gebaeude_t::calc_image()
{
	grund_t *gr = welt->lookup( get_pos() );
	// need no ground?
	if(  remove_ground  &&  gr->get_typ() == grund_t::fundament  ) {
		gr->set_image( IMG_EMPTY );
	}

	static uint8 effective_season[][5] = { {0,0,0,0,0}, {0,0,0,0,1}, {0,0,0,0,1}, {0,1,2,3,2}, {0,1,2,3,4} };  // season image lookup from [number of images] and [actual season/snow]

	if(  (gr  &&  gr->ist_tunnel()  &&  !gr->ist_karten_boden())  ||  tile->get_seasons() < 2  ) {
		season = 0;
	}
	else if(  get_pos().z - (get_yoff() / TILE_HEIGHT_STEP) >= welt->get_snowline()  ||  welt->get_climate( get_pos().get_2d() ) == arctic_climate  ) {
		// snowy winter graphics
		season = effective_season[tile->get_seasons() - 1][4];
	}
	else if(  get_pos().z - (get_yoff() / TILE_HEIGHT_STEP) >= welt->get_snowline() - 1  &&  welt->get_season() == 0  ) {
		// snowline crossing in summer
		// so at least some weeks spring/autumn
		season = effective_season[tile->get_seasons() - 1][welt->get_last_month() <= 5 ? 3 : 1];
	}
	else {
		season = effective_season[tile->get_seasons() - 1][welt->get_season()];
	}

	background_animated = tile->is_background_animated( season );
}


image_id gebaeude_t::get_image() const
{
	if(env_t::hide_buildings!=0  &&  tile->has_image()) {
		// opaque houses
		if (is_city_building()) {
			return env_t::hide_with_transparency ? skinverwaltung_t::fussweg->get_image_id(0) : skinverwaltung_t::construction_site->get_image_id(0);
		}
		else if(  (env_t::hide_buildings == env_t::ALL_HIDDEN_BUILDING  &&  tile->get_desc()->get_type() < building_desc_t::others)) {
			// hide with transparency or tile without information
			if (env_t::hide_with_transparency) {
				if (tile->get_desc()->get_type() == building_desc_t::factory  &&  ptr.fab->get_desc()->get_placement() == factory_desc_t::Water) {
					// no ground tiles for water things
					return IMG_EMPTY;
				}
				return skinverwaltung_t::fussweg->get_image_id(0);
			}
			else {
				uint16 kind=skinverwaltung_t::construction_site->get_count()<=tile->get_desc()->get_type() ? skinverwaltung_t::construction_site->get_count()-1 : tile->get_desc()->get_type();
				return skinverwaltung_t::construction_site->get_image_id( kind );
			}
		}
	}

	// winter for buildings only above snowline
	if (show_construction) {
		return skinverwaltung_t::construction_site->get_image_id(0);
	}
	else {
		return tile->get_background( anim_frame, 0, season );
	}
}


image_id gebaeude_t::get_outline_image() const
{
	if(env_t::hide_buildings!=0  &&  env_t::hide_with_transparency  &&  !show_construction) {
		// opaque houses
		return tile->get_background( anim_frame, 0, season );
	}
	return IMG_EMPTY;
}


/* gives outline colour and plots background tile if needed for transparent view */
FLAGGED_PIXVAL gebaeude_t::get_outline_colour() const
{
	uint8 colours[] = { COL_BLACK, COL_YELLOW, COL_YELLOW, COL_PURPLE, COL_RED, COL_GREEN };
	FLAGGED_PIXVAL disp_colour = 0;
	if(env_t::hide_buildings!=env_t::NOT_HIDE && env_t::hide_with_transparency) {
		if(is_city_building()) {
			disp_colour = color_idx_to_rgb(colours[0]) | TRANSPARENT50_FLAG | OUTLINE_FLAG;
		}
		else if (env_t::hide_buildings == env_t::ALL_HIDDEN_BUILDING && tile->get_desc()->get_type() < building_desc_t::others) {
			// special building
			disp_colour = color_idx_to_rgb(colours[tile->get_desc()->get_type()]) | TRANSPARENT50_FLAG | OUTLINE_FLAG;
		}
	}
	return disp_colour;
}


image_id gebaeude_t::get_image(int nr) const
{
	if (show_construction || env_t::hide_buildings) {
		return IMG_EMPTY;
	}
	else {
		return tile->get_background( anim_frame, nr, season );
	}
}


image_id gebaeude_t::get_front_image() const
{
	if (show_construction) {
		return IMG_EMPTY;
	}
	if (env_t::hide_buildings != 0   &&  (is_city_building()  ||  (env_t::hide_buildings == env_t::ALL_HIDDEN_BUILDING  &&  tile->get_desc()->get_type() < building_desc_t::others))) {
		return IMG_EMPTY;
	}
	else {
		// Show depots, station buildings etc.
		return tile->get_foreground( anim_frame, season );
	}
}
/**
* @return eigener Name oder Name der Fabrik falls Teil einer Fabrik
*/
const char *gebaeude_t::get_name() const
{
	if (is_factory  &&  ptr.fab) {
		return ptr.fab->get_name();
	}

	switch (tile->get_desc()->get_type()) {
				case building_desc_t::attraction_city:   return "Besonderes Gebaeude";
				case building_desc_t::attraction_land:   return "Sehenswuerdigkeit";
				case building_desc_t::monument:           return "Denkmal";
				case building_desc_t::townhall:           return "Rathaus";
				case building_desc_t::signalbox:
				case building_desc_t::depot:			  return tile->get_desc()->get_name();
				default: break;
	}
	return "Gebaeude";
}

const char* gebaeude_t::get_individual_name() const
{
	if (is_factory && ptr.fab)
	{
		return ptr.fab->get_name();
	}
	else if (tile)
	{
		return tile->get_desc()->get_name();
	}
	else
	{
		return get_name();
	}
}

/**
* waytype associated with this object
*/
waytype_t gebaeude_t::get_waytype() const
{
	const building_desc_t *desc = tile->get_desc();
	waytype_t wt = invalid_wt;

	const building_desc_t::btype type = tile->get_desc()->get_type();
	if (type == building_desc_t::depot || type == building_desc_t::generic_stop || type == building_desc_t::generic_extension) {
		wt = (waytype_t)desc->get_extra();
	}
	return wt;
}


bool gebaeude_t::is_townhall() const
{
	return tile->get_desc()->is_townhall();
}


bool gebaeude_t::is_monument() const
{
	return tile->get_desc()->get_type() == building_desc_t::monument;
}


bool gebaeude_t::is_headquarter() const
{
	return tile->get_desc()->is_headquarters();
}

bool gebaeude_t::is_attraction() const
{
	return tile->get_desc()->is_attraction();
}


bool gebaeude_t::is_city_building() const
{
	return tile->get_desc()->is_city_building();
}

bool gebaeude_t::is_signalbox() const
{
	return tile->get_desc()->is_signalbox();
}


void gebaeude_t::show_info()
{
	if (get_fabrik()) {
		ptr.fab->show_info();
		return;
	}

	const uint32 old_count = win_get_open_count();

	if (is_headquarter()) {
		create_win( new headquarter_info_t(get_owner()), w_info, magic_headquarter+get_owner()->get_player_nr() );
		return;
	}
	else if (is_townhall()) {
		get_stadt()->show_info();
	}
	else {
		create_win(new building_info_t(access_first_tile(), get_owner()), w_info, (ptrdiff_t)access_first_tile());
	}

	if (!tile->get_desc()->no_info_window()) {
		if( env_t::townhall_info  &&  old_count==win_get_open_count() ) {
			// open info window for the first tile of our building (not relying on presence of (0,0) tile)
			create_win(new building_info_t(access_first_tile(), get_owner()), w_info, (ptrdiff_t)access_first_tile());
		}
	}
}

bool gebaeude_t::is_same_building(gebaeude_t* other) const
{
	return (other != NULL) && (get_tile()->get_desc() == other->get_tile()->get_desc())
		&& (get_first_tile() == other->get_first_tile());
}

bool gebaeude_t::is_within_players_network(const player_t* player, uint8 catg_index) const
{
	const planquadrat_t* plan = welt->access(get_pos().get_2d());

	if (plan->get_haltlist_count() > 0) {
		const nearby_halt_t *const halt_list = plan->get_haltlist();
		for (int h = 0; h < plan->get_haltlist_count(); h++)
		{
			const halthandle_t halt = halt_list[h].halt;
			if (halt->get_owner() == player && catg_index == goods_manager_t::INDEX_NONE) {
				return true;
			}
			if (halt->has_available_network(player, catg_index))
			{
				return true;
			}
		}
	}
	return false;
}


const gebaeude_t* gebaeude_t::get_first_tile() const
{
	if (tile)
	{
		const building_desc_t* const building_desc = tile->get_desc();
		const uint8 layout = tile->get_layout();
		koord k;
		for (k.x = 0; k.x<building_desc->get_x(layout); k.x++) {
			for (k.y = 0; k.y<building_desc->get_y(layout); k.y++) {
				const building_tile_desc_t *tile = building_desc->get_tile(layout, k.x, k.y);
				if (tile == NULL || !tile->has_image()) {
					continue;
				}
				if (grund_t *gr = welt->lookup(get_pos() - get_tile()->get_offset() + k))
				{
					gebaeude_t* gb;
					if (tile->get_desc()->is_signalbox())
					{
						gb = gr->get_signalbox();
					}
					else
					{
						gb = gr->find<gebaeude_t>();
					}
					if (gb && gb->get_tile() == tile)
					{
						return gb;
					}
				}
			}
		}
	}
	return this;
}

gebaeude_t* gebaeude_t::access_first_tile()
{
	if (tile)
	{
		const building_desc_t* const building_desc = tile->get_desc();
		const uint8 layout = tile->get_layout();
		koord k;
		for (k.x = 0; k.x<building_desc->get_x(layout); k.x++) {
			for (k.y = 0; k.y<building_desc->get_y(layout); k.y++) {
				const building_tile_desc_t *tile = building_desc->get_tile(layout, k.x, k.y);
				if (tile == NULL || !tile->has_image()) {
					continue;
				}
				if (grund_t *gr = welt->lookup(get_pos() - get_tile()->get_offset() + k))
				{
					gebaeude_t* gb;
					if (tile->get_desc()->is_signalbox())
					{
						gb = gr->get_signalbox();
					}
					else
					{
						gb = gr->find<gebaeude_t>();
					}
					if (gb && gb->get_tile() == tile)
					{
						return gb;
					}
				}
			}
		}
	}
	return this;
}

void gebaeude_t::get_description(cbuffer_t & buf) const
{
	if(is_factory  &&  ptr.fab != NULL) {
		buf.append(ptr.fab->get_name());
	}
	else if(show_construction) {
		buf.append(translator::translate("Baustelle"));
		buf.append("\n");
	}
	else {
		const char *desc = tile->get_desc()->get_name();
		if(desc != NULL) {
			const char *trans_desc = translator::translate(desc);
			if(trans_desc==desc) {
				// no description here
				switch(tile->get_desc()->get_type()) {
					case building_desc_t::city_res:
						trans_desc = translator::translate("residential house");
						break;
					case building_desc_t::city_ind:
						trans_desc = translator::translate("industrial building");
						break;
					case building_desc_t::city_com:
						trans_desc = translator::translate("shops and stores");
						break;
					default:
						// use file name
						break;
				}
				buf.append(trans_desc);
			}
			else {
				// since the format changed, we remove all but double newlines
				char *text = new char[strlen(trans_desc)+1];
				char *dest = text;
				const char *src = trans_desc;
				while(  *src!=0  ) {
					*dest = *src;
					if(src[0]=='\n') {
						if(src[1]=='\n') {
							src ++;
							dest++;
							*dest = '\n';
						}
						else {
							*dest = ' ';
						}
					}
					src ++;
					dest ++;
				}
				// remove double line breaks at the end
				*dest = 0;
				while( dest>text  &&  *--dest=='\n'  ) {
					*dest = 0;
				}

				buf.append(text);
				delete [] text;
			}
		}
		else
		{
			buf.append("unknown");
		}
	}
}


void gebaeude_t::info(cbuffer_t & buf) const
{
	obj_t::info(buf);

	get_description(buf);

	if (!is_factory && !show_construction)
	{
		buf.append("\n");

		// belongs to which city?
		if (get_stadt() != NULL)
		{
			buf.printf(translator::translate("Town: %s\n"), ptr.stadt->get_name());
			buf.append("\n");
		}

		if (get_tile()->get_desc()->get_type() == building_desc_t::city_res)
		{
			buf.printf("%s: %d\n", translator::translate("citicens"), get_adjusted_population());
		}
		buf.printf("%s: %d\n", translator::translate("Visitor demand"), get_adjusted_visitor_demand());
#ifdef DEBUG
		buf.printf("%s (%s): %d (%d)\n", translator::translate("Jobs"), translator::translate("available"), get_adjusted_jobs(), check_remaining_available_jobs());
#else
		buf.printf("%s (%s): %d (%d)\n", translator::translate("Jobs"), translator::translate("available"), get_adjusted_jobs(), max(0, check_remaining_available_jobs()));
#endif
		buf.printf("%s: %d\n", translator::translate("Mail demand/output"), get_adjusted_mail_demand());

        buf.printf("%s: %s\n", translator::translate("Built in"), translator::get_year_month(purchase_time));

		building_desc_t const& h = *tile->get_desc();

		buf.printf("%s%u", translator::translate("\nBauzeit von"), h.get_intro_year_month() / 12);
		if (h.get_retire_year_month() != DEFAULT_RETIRE_DATE * 12) {
			buf.printf("%s%u", translator::translate("\nBauzeit bis"), h.get_retire_year_month() / 12);
		}
		buf.append("\n");
		if (get_owner() == NULL) {
			buf.append(translator::translate("Wert"));
			buf.append(": ");
			// The land value calculation below will need modifying if multi-tile city buildings are ever introduced.
			sint64 cost = welt->get_settings().cst_multiply_remove_haus*2 * tile->get_desc()->get_level()*tile->get_desc()->get_size().x*tile->get_desc()->get_size().y;
			cost += welt->get_land_value(get_pos());
			buf.append(-(cost/100.0),2);
			buf.append("$\n");
		}

		if (char const* const maker = tile->get_desc()->get_copyright()) {
			buf.printf(translator::translate("Constructed by %s"), maker);
		}

	}
}

void gebaeude_t::display_coverage_radius(bool display)
{
	gebaeude_t* gb = (gebaeude_t*)welt->get_active_player()->get_selected_signalbox();
	if (gb)
	{
		if (is_signalbox())
		{
			uint32 const radius = gb->get_tile()->get_desc()->get_radius();
			uint16 const cov = radius / welt->get_settings().get_meters_per_tile();
			for (int x = 0; x <= cov * 2; x++)
			{
				for (int y = 0; y <= cov * 2; y++)
				{
					koord gb_pos = koord(gb->get_pos().get_2d());
					koord check_pos = koord(gb_pos.x - cov + x, gb_pos.y - cov + y);
					// Mark a 5x5 cross at center of circle
					if (shortest_distance(gb_pos, check_pos) <= cov)
					{
						if ((check_pos.x == gb->get_pos().x && (check_pos.y >= gb->get_pos().y - 2 && check_pos.y <= gb->get_pos().y + 2)) || (check_pos.y == gb->get_pos().y && (check_pos.x >= gb->get_pos().x - 2 && check_pos.x <= gb->get_pos().x + 2)))
						{
							welt->mark_area(koord3d(check_pos, gb->get_pos().z), koord(1, 1), display);
						}
						// Mark the circle
						if (shortest_distance(gb_pos, check_pos) >= cov)
						{
							welt->mark_area(koord3d(check_pos, gb->get_pos().z), koord(1, 1), display);
						}
					}
				}
			}
		}
	}
}


void gebaeude_t::new_year()
{
	if (get_tile()->get_desc()->get_type() == building_desc_t::city_res)
	{
		passenger_success_percent_last_year_commuting = get_passenger_success_percent_this_year_commuting();
		passenger_success_percent_last_year_visiting = get_passenger_success_percent_this_year_visiting();
	}
	else
	{
		// For non-residential buildings, these numbers are used to record only absolute numbers of visitors/commuters.
		// Accordingly, we do not make use of "generated" numbers, and the "succeeded" figures are actually records of
		// absolute numbers of visitors/commuters. Accordingly, the last year percent figures must also store the
		// absolute number of visitors/commuters rather than a percentage.
		passenger_success_percent_last_year_commuting = passengers_succeeded_commuting;
		passenger_success_percent_last_year_visiting = passengers_succeeded_visiting;
	}
	mail_delivery_succeeded_last_year = mail_delivery_succeeded;
	mail_delivery_success_percent_last_year = get_mail_delivery_success_percent_this_year();

	passengers_succeeded_commuting = passengers_generated_commuting = passengers_succeeded_visiting = passengers_generated_visiting = mail_generated = mail_delivery_succeeded = 0;
}


void gebaeude_t::rdwr(loadsave_t *file)
{
	xml_tag_t d(file, "gebaeude_t");

	obj_t::rdwr(file);

	char buf[128];
	short idx;

	if (file->is_saving()) {
		const char *s = tile->get_desc()->get_name();
		file->rdwr_str(s);
		idx = tile->get_index();
	}
	else {
		file->rdwr_str(buf, lengthof(buf));
	}
	file->rdwr_short(idx);
	if (file->get_extended_version() <= 1)
	{
		uint32 old_construction_start = (uint32)construction_start;
		file->rdwr_long(old_construction_start);
		construction_start = old_construction_start;
	}
	else
	{
		sint64 month_start = (purchase_time - welt->get_settings().get_starting_month() - welt->get_settings().get_starting_year()*12) *
			welt->ticks_per_world_month;
		file->rdwr_longlong(file->is_saving() ? month_start : construction_start);
	}

	if (!file->is_saving()) { // stepping year in game results in mismatch of Ticks vs. Year/Month; avoid updating here
		purchase_time = (construction_start / welt->ticks_per_world_month)+welt->get_settings().get_starting_month()+
			welt->get_settings().get_starting_year()*12;
	}

	if (file->get_extended_version() >= 12)
	{
		file->rdwr_longlong(available_jobs_by_time);
	}

	if (file->is_loading()) {
		tile = hausbauer_t::find_tile(buf, idx);
		if (tile == NULL) {
			// try with compatibility list first
			tile = hausbauer_t::find_tile(translator::compatibility_name(buf), idx);
			if (tile == NULL) {
				DBG_MESSAGE("gebaeude_t::rdwr()", "neither %s nor %s, tile %i not found, try other replacement", translator::compatibility_name(buf), buf, idx);
			}
			else {
				DBG_MESSAGE("gebaeude_t::rdwr()", "%s replaced by %s, tile %i", buf, translator::compatibility_name(buf), idx);
			}
		}
		if (tile == NULL) {
			// first check for special buildings
			if (strstr(buf, "TrainStop") != NULL) {
				tile = hausbauer_t::find_tile("TrainStop", idx);
			}
			else if (strstr(buf, "BusStop") != NULL) {
				tile = hausbauer_t::find_tile("BusStop", idx);
			}
			else if (strstr(buf, "ShipStop") != NULL) {
				tile = hausbauer_t::find_tile("ShipStop", idx);
			}
			else if (strstr(buf, "PostOffice") != NULL) {
				tile = hausbauer_t::find_tile("PostOffice", idx);
			}
			else if (strstr(buf, "StationBlg") != NULL) {
				tile = hausbauer_t::find_tile("StationBlg", idx);
			}
			else {
				// try to find a fitting building
				int level = atoi(buf);
				building_desc_t::btype type = building_desc_t::unknown;

				if (level>0) {
					// May be an old 64er, so we can try some
					if (strncmp(buf + 3, "WOHN", 4) == 0) {
						type = building_desc_t::city_res;
					}
					else if (strncmp(buf + 3, "FAB", 3) == 0) {
						type = building_desc_t::city_ind;
					}
					else {
						type = building_desc_t::city_com;
					}
					level--;
				}
				else if (buf[3] == '_') {
					/* should have the form of RES/IND/COM_xx_level
					* xx is usually a number by can be anything without underscores
					*/
					level = atoi(strrchr(buf, '_') + 1);
					if (level>0) {
						switch (toupper(buf[0])) {
						case 'R': type = building_desc_t::city_res; break;
						case 'I': type = building_desc_t::city_ind; break;
						case 'C': type = building_desc_t::city_com; break;
						}
					}
					level--;
				}
				// we try to replace citybuildings with their matching counterparts
				// if none are matching, we try again without climates and timeline!
				// only 1x1 buildings can fill the empty tile to avoid overlap.
				const koord single(1,1);
				switch (type) {
				case building_desc_t::city_res:
				{
					const building_desc_t *bdsc = hausbauer_t::get_residential(level, single, welt->get_timeline_year_month(), welt->get_climate_at_height(get_pos().z), welt->get_region(get_pos().get_2d()));
					if (bdsc == NULL) {
						bdsc = hausbauer_t::get_residential(level, single, 0, MAX_CLIMATES, 0xFFu);
					}
					if (bdsc) {
						dbg->message("gebaeude_t::rwdr", "replace unknown building %s with residence level %i by %s", buf, level, bdsc->get_name());
						tile = bdsc->get_tile(0);
					}
				}
				break;

				case building_desc_t::city_com:
				{
					const building_desc_t *bdsc = hausbauer_t::get_commercial(level, single, welt->get_timeline_year_month(), welt->get_climate_at_height(get_pos().z), welt->get_region(get_pos().get_2d()));
					if (bdsc == NULL) {
						bdsc = hausbauer_t::get_commercial(level, single, 0, MAX_CLIMATES, 0xFFu);
					}
					if (bdsc) {
						dbg->message("gebaeude_t::rwdr", "replace unknown building %s with commercial level %i by %s", buf, level, bdsc->get_name());
						tile = bdsc->get_tile(0);
					}
				}
				break;

				case building_desc_t::city_ind:
				{
					const building_desc_t *bdsc = hausbauer_t::get_industrial(level, single, welt->get_timeline_year_month(), welt->get_climate_at_height(get_pos().z), welt->get_region(get_pos().get_2d()));
					if (bdsc == NULL) {
						bdsc = hausbauer_t::get_industrial(level, single, 0, MAX_CLIMATES, 0xFFu);
						if (bdsc == NULL) {
							bdsc = hausbauer_t::get_residential(level, single, 0, MAX_CLIMATES, 0xFFu);
						}
					}
					if (bdsc) {
						dbg->message("gebaeude_t::rwdr", "replace unknown building %s with industrie level %i by %s", buf, level, bdsc->get_name());
						tile = bdsc->get_tile(0);
					}
				}
				break;

				default:
					dbg->warning("gebaeude_t::rwdr", "description %s for building at %d,%d not found (will be removed)!", buf, get_pos().x, get_pos().y);
					welt->add_missing_paks(buf, karte_t::MISSING_BUILDING);
				}
			}
		}

		// here we should have a valid tile pointer or nothing ...

			/* avoid double construction of monuments:
			* remove them from selection lists
			*/
		if (tile  &&  tile->get_desc()->get_type() == building_desc_t::monument) {
			hausbauer_t::monument_erected(tile->get_desc());
		}
		if (tile) {
			remove_ground = tile->has_image() && !tile->get_desc()->needs_ground();
		}
	}

	if(file->is_version_less(99, 6)) {
		// ignore the sync flag
		uint8 dummy = sync;
		file->rdwr_byte(dummy);
	}

	if (file->get_extended_version() >= 12)
	{
		bool f = is_factory;
		file->rdwr_bool(f);
		is_factory = f;
	}

	// restore city pointer here
	if(  file->is_version_atleast(99, 14) && !is_factory  ) {
		sint32 city_index = -1;
		if (file->is_saving() && ptr.stadt != NULL)
		{
			if (welt->get_cities().is_contained(ptr.stadt))
			{
				city_index = welt->get_cities().index_of(ptr.stadt);
			}
			else
			{
				// Reaching here means that the city has been deleted.
				ptr.stadt = NULL;
			}
		}
		file->rdwr_long(city_index);
		if (file->is_loading() && city_index != -1 && (tile == NULL || tile->get_desc() == NULL || tile->get_desc()->is_connected_with_town())) {
			ptr.stadt = welt->get_cities()[city_index];
		}
	}

	if (file->get_extended_version() >= 11)
	{
		file->rdwr_short(passengers_generated_commuting);
		file->rdwr_short(passengers_succeeded_commuting);
		if (file->get_extended_version() < 12)
		{
			uint8 old_success_percent_commuting = passenger_success_percent_last_year_commuting;
			file->rdwr_byte(old_success_percent_commuting);
			passenger_success_percent_last_year_commuting = old_success_percent_commuting;
		}
		else
		{
			file->rdwr_short(passenger_success_percent_last_year_commuting);
		}

		file->rdwr_short(passengers_generated_visiting);
		file->rdwr_short(passengers_succeeded_visiting);
		if (file->get_extended_version() < 12)
		{
			uint8 old_success_percent_visiting = passenger_success_percent_last_year_visiting;
			file->rdwr_byte(old_success_percent_visiting);
			passenger_success_percent_last_year_visiting = old_success_percent_visiting;
		}
		else
		{
			file->rdwr_short(passenger_success_percent_last_year_visiting);
		}
	}

	if (file->get_extended_version() >= 12)
	{
		file->rdwr_short(people.population); // No need to distinguish the parts of the union here.
		file->rdwr_short(jobs);
		file->rdwr_short(mail_demand);
	}

	if ((file->get_extended_version() == 13 && file->get_extended_revision() >= 1) || file->get_extended_version() >= 14)
	{
		loaded_passenger_and_mail_figres = true;

		file->rdwr_short(jobs);
		file->rdwr_short(people.visitor_demand);
		file->rdwr_short(mail_demand);

		file->rdwr_short(adjusted_jobs);
		file->rdwr_short(adjusted_people.visitor_demand);
		file->rdwr_short(adjusted_mail_demand);
	}

	if ((file->get_extended_version() == 14 && file->get_extended_revision() >= 4) || file->get_extended_version() >= 15)
	{
		file->rdwr_short(mail_generated);
		file->rdwr_short(mail_delivery_succeeded_last_year);
		file->rdwr_short(mail_delivery_succeeded);
		file->rdwr_short(mail_delivery_success_percent_last_year);
	}

	if (file->is_loading() && tile)
	{
		anim_frame = 0;
		anim_time = 0;
		sync = false;

		const building_desc_t* building_type = tile->get_desc();

		if (!loaded_passenger_and_mail_figres)
		{
			if (building_type->get_type() == building_desc_t::city_res)
			{
				people.population = building_type->get_population_and_visitor_demand_capacity() == 65535 ? building_type->get_level() * welt->get_settings().get_population_per_level() : building_type->get_population_and_visitor_demand_capacity();
				adjusted_people.population = welt->calc_adjusted_monthly_figure(people.population);
				if (people.population > 0 && adjusted_people.population == 0)
				{
					adjusted_people.population = 1;
				}
			}
			else if (building_type->get_type() == building_desc_t::city_ind)
			{
				people.visitor_demand = adjusted_people.visitor_demand = 0;
			}
			else
			{
				people.visitor_demand = building_type->get_population_and_visitor_demand_capacity() == 65535 ? building_type->get_level() * welt->get_settings().get_visitor_demand_per_level() : building_type->get_population_and_visitor_demand_capacity();
				adjusted_people.visitor_demand = welt->calc_adjusted_monthly_figure(people.visitor_demand);
				if (people.visitor_demand > 0 && adjusted_people.visitor_demand == 0)
				{
					adjusted_people.visitor_demand = 1;
				}
			}

			jobs = building_type->get_employment_capacity() == 65535 ? (is_monument() || building_type->get_type() == building_desc_t::city_res) ? 0 : building_type->get_level() * welt->get_settings().get_jobs_per_level() : building_type->get_employment_capacity();
			mail_demand = building_type->get_mail_demand_and_production_capacity() == 65535 ? is_monument() ? 0 : building_type->get_level() * welt->get_settings().get_mail_per_level() : building_type->get_mail_demand_and_production_capacity();

			adjusted_jobs = welt->calc_adjusted_monthly_figure(jobs);
			if (jobs > 0 && adjusted_jobs == 0)
			{
				adjusted_jobs = 1;
			}

			adjusted_mail_demand = welt->calc_adjusted_monthly_figure(mail_demand);
			if (mail_demand > 0 && adjusted_mail_demand == 0)
			{
				adjusted_mail_demand = 1;
			}
		}

		// rebuild tourist attraction list
		if (tile && building_type->is_attraction())
		{
			welt->add_attraction(this);
		}

		if (is_in_world_list == 0)
		{
			// Do not add this to the world list when loading a building from a factory,
			// as this needs to be taken out of the world list again, and this increases
			// loading time considerably.

			// Add this here: there is no advantage to adding buildings multi-threadedly
			// to a single list, especially when that requires an insertion sort, and
			// adding it here, single-threadedly, does not.

			welt->add_building_to_world_list(this);
		}
	}
}


void gebaeude_t::finish_rd()
{
	calc_image();
	sint64 maint = tile->get_desc()->get_maintenance();
	if (maint == PRICE_MAGIC)
	{
		maint = welt->get_settings().maint_building*tile->get_desc()->get_level();
	}
	player_t::add_maintenance(get_owner(), maint, tile->get_desc()->get_finance_waytype());

	// citybuilding, but no town?
	if (tile->get_offset() == koord(0, 0))
	{
		if (tile->get_desc()->is_connected_with_town())
		{
			stadt_t *city = (ptr.stadt == NULL) ? welt->find_nearest_city(get_pos().get_2d()) : ptr.stadt;
			if (city)
			{
				if (!is_factory && ptr.stadt == NULL)
				{
					// This will save much time in looking this up when generating passengers/mail.
					ptr.stadt = welt->get_city(get_pos().get_2d());
				}
#ifdef MULTI_THREAD
				pthread_mutex_lock(&add_to_city_mutex);
#endif
				city->add_gebaeude_to_stadt(this, env_t::networkmode, true, false);
#ifdef MULTI_THREAD
				pthread_mutex_unlock(&add_to_city_mutex);
#endif
			}
		}
		else if (!is_factory && ptr.stadt == NULL)
		{
			// This will save much time in looking this up when generating passengers/mail.
			ptr.stadt = welt->get_city(get_pos().get_2d());
		}
	}
	else if(!is_factory && ptr.stadt == NULL)
	{
		// This will save much time in looking this up when generating passengers/mail.
		ptr.stadt = welt->get_city(get_pos().get_2d());
	}
	set_building_tiles();
}


void gebaeude_t::cleanup(player_t *player)
{
//	DBG_MESSAGE("gebaeude_t::cleanup()","gb %i");
	// remove costs

	const building_desc_t* desc = tile->get_desc();
	sint64 cost = 0;

	if (desc->is_transport_building() || desc->is_signalbox())
	{
		if (desc->get_price() != PRICE_MAGIC)
		{
			cost = -desc->get_price() / 2;
		}
		else
		{
			cost = welt->get_settings().cst_multiply_remove_haus * (desc->get_level());
		}

		// If the player does not own the building, the land is not bought by bulldozing, so do not add the purchase cost.
		// (A player putting a marker on the tile will have to pay to buy the land again).
		// If the player does already own the building, the player is refunded the empty tile cost, as bulldozing a tile with a building
		// means that the player no longer owns the tile, and will have to pay again to purcahse it.

		if (player != get_owner() && desc->get_type() != building_desc_t::generic_stop) // A stop is built on top of a way, so building one does not require buying land, and, likewise, removing one does not involve releasing land.
		{
			const sint64 land_value = abs(welt->get_land_value(get_pos()) * desc->get_size().x * desc->get_size().y);
			player_t::book_construction_costs(get_owner(), land_value + cost, get_pos().get_2d(), tile->get_desc()->get_finance_waytype());
		}
		else
		{
			player_t::book_construction_costs(player, cost, get_pos().get_2d(), tile->get_desc()->get_finance_waytype());
		}
	}
	else
	{
		// Station buildings (not extension buildings, handled elsewhere) are built over existing ways, so no need to account for the land cost.

		// tearing down halts is always single costs only
		cost = desc->get_price();
		// This check is necessary because the number of PRICE_MAGIC is used if no price is specified.
		if (desc->get_base_price() == PRICE_MAGIC)
		{
			if (desc->is_city_building()) {
				cost = welt->get_settings().cst_multiply_remove_haus * desc->get_level();
			}
			else {
				// TODO: find a way of checking what *kind* of stop that this is. This assumes railway.
				cost = welt->get_settings().cst_multiply_station * desc->get_level();
				// Should be cheaper to bulldoze than build.
				cost /= 2;
			}
		}
		else {
			// Should be cheaper to bulldoze than build.
			cost /= 2;
		}

		// However, the land value is restored to the player who, by bulldozing, is relinquishing ownership of the land if there are not already ways on the land.
		// Note: Cost and land value are negative numbers here.

		const sint64 land_value = welt->get_land_value(get_pos()) * desc->get_size().x * desc->get_size().y;
		if (welt->lookup(get_pos()) && !welt->lookup(get_pos())->get_weg_nr(0))
		{
			if (player == get_owner())
			{
				cost += land_value;
			}
			else
			{
				player_t::book_construction_costs(get_owner(), land_value, get_pos().get_2d(), tile->get_desc()->get_finance_waytype());
			}
		}
		player_t::book_construction_costs(player, cost, get_pos().get_2d(), tile->get_desc()->get_finance_waytype());
	}

	// may need to update next buildings, in the case of start, middle, end buildings
	if (tile->get_desc()->get_all_layouts()>1 && !is_city_building()) {

		// realign surrounding buildings...
		uint32 layout = tile->get_layout();

		// detect if we are connected at far (north/west) end
		grund_t * gr = welt->lookup(get_pos());
		if (gr) {
			sint8 offset = gr->get_weg_yoff() / TILE_HEIGHT_STEP;
			gr = welt->lookup(get_pos() + koord3d((layout & 1 ? koord::east : koord::south), offset));
			if (!gr) {
				// check whether bridge end tile
				grund_t * gr_tmp = welt->lookup(get_pos() + koord3d((layout & 1 ? koord::east : koord::south), offset - 1));
				if (gr_tmp && gr_tmp->get_weg_yoff() / TILE_HEIGHT_STEP == 1) {
					gr = gr_tmp;
				}
			}
			if (gr) {
				gebaeude_t* gb = gr->find<gebaeude_t>();
				if (gb  &&  gb->get_tile()->get_desc()->get_all_layouts()>4u) {
					koord xy = gb->get_tile()->get_offset();
					uint8 layoutbase = gb->get_tile()->get_layout();
					if ((layoutbase & 1u) == (layout & 1u)) {
						layoutbase |= 4u; // set far bit on neighbour
						gb->set_tile(gb->get_tile()->get_desc()->get_tile(layoutbase, xy.x, xy.y), false);
					}
				}
			}

			// detect if near (south/east) end
			gr = welt->lookup(get_pos() + koord3d((layout & 1 ? koord::west : koord::north), offset));
			if (!gr) {
				// check whether bridge end tile
				grund_t * gr_tmp = welt->lookup(get_pos() + koord3d((layout & 1 ? koord::west : koord::north), offset - 1));
				if (gr_tmp && gr_tmp->get_weg_yoff() / TILE_HEIGHT_STEP == 1) {
					gr = gr_tmp;
				}
			}
			if (gr) {
				gebaeude_t* gb = gr->find<gebaeude_t>();
				if (gb  &&  gb->get_tile()->get_desc()->get_all_layouts()>4) {
					koord xy = gb->get_tile()->get_offset();
					uint8 layoutbase = gb->get_tile()->get_layout();
					if ((layoutbase & 1u) == (layout & 1u)) {
						layoutbase |= 2u; // set near bit on neighbour
						gb->set_tile(gb->get_tile()->get_desc()->get_tile(layoutbase, xy.x, xy.y), false);
					}
				}
			}
		}
	}
	mark_images_dirty();
}


void gebaeude_t::mark_images_dirty() const
{
	// remove all traces from the screen
	image_id img;
	if (  show_construction  ||
			(!env_t::hide_with_transparency  &&
				env_t::hide_buildings>(is_city_building() ? env_t::NOT_HIDE : env_t::SOME_HIDDEN_BUILDING))  ) {
		img = skinverwaltung_t::construction_site->get_image_id(0);
	}
	else {
		img = tile->get_background( anim_frame, 0, season ) ;
	}
	for(  int i=0;  img!=IMG_EMPTY;  img=get_image(++i)  ) {
		mark_image_dirty( img, -(i*get_tile_raster_width()) );
	}
}

uint16 gebaeude_t::get_weight() const
{
	return tile->get_desc()->get_level();
}

void gebaeude_t::set_commute_trip(uint16 number)
{
	// Record the number of arriving workers by encoding the earliest time at which new workers can arrive.
	const sint64 job_ticks = ((sint64)number * welt->get_settings().get_job_replenishment_ticks()) / ((sint64)adjusted_jobs < 1ll ? 1ll : (sint64)adjusted_jobs);
	const sint64 new_jobs_by_time = welt->get_ticks() - welt->get_settings().get_job_replenishment_ticks();
	available_jobs_by_time = std::max(new_jobs_by_time + job_ticks, available_jobs_by_time + job_ticks);
	add_passengers_succeeded_commuting(number);
}


uint16 gebaeude_t::get_adjusted_population() const
{
	return tile->get_desc()->get_type() == building_desc_t::city_res ? adjusted_people.population : 0;
}

uint16 gebaeude_t::get_adjusted_population_by_class(uint8 p_class) const
{
	if (get_tile()->get_desc()->get_type() != building_desc_t::city_res) {
		return 0;
	}
	const uint8 pass_classes = goods_manager_t::passengers->get_number_of_classes();
	const uint32 class_proportions_sum = get_tile()->get_desc()->get_class_proportions_sum();
	if (class_proportions_sum == 0) {
		return adjusted_people.population / pass_classes;
	}
	if (p_class > pass_classes-1) {
		return adjusted_people.population; // error
	}
	switch (p_class) {
		case -1:
			return adjusted_people.population;
		case 0:
			return adjusted_people.population * tile->get_desc()->get_class_proportion(p_class) / class_proportions_sum;
		default:
			return adjusted_people.population * tile->get_desc()->get_class_proportion(p_class) / class_proportions_sum - adjusted_people.population * tile->get_desc()->get_class_proportion(p_class-1) / class_proportions_sum;
	}
	return 0;
}

uint16 gebaeude_t::get_visitor_demand() const
{
	if (tile->get_desc()->get_type() != building_desc_t::city_res)
	{
		return people.visitor_demand;
	}

	uint16 reduced_demand = people.population / 20;
	return reduced_demand > 0 ? reduced_demand : 1;
}

uint16 gebaeude_t::get_adjusted_visitor_demand() const
{
	if (tile->get_desc()->get_type() != building_desc_t::city_res)
	{
		return adjusted_people.visitor_demand;
	}

	uint16 reduced_demand = adjusted_people.population / 20;
	return reduced_demand > 0 ? reduced_demand : 1;
}

uint16 gebaeude_t::get_adjusted_visitor_demand_by_class(uint8 p_class) const
{
	if (get_tile()->get_desc()->get_type() == building_desc_t::city_res) {
		return 0; // Tentatively ignore. Mostly less than 1
	}
	const uint8 pass_classes = goods_manager_t::passengers->get_number_of_classes();
	const uint32 class_proportions_sum = get_tile()->get_desc()->get_class_proportions_sum();
	if (class_proportions_sum == 0) {
		return adjusted_people.visitor_demand / pass_classes;
	}
	if (p_class > pass_classes - 1) {
		return adjusted_people.visitor_demand; // error
	}
	switch (p_class) {
		case -1:
			return adjusted_people.visitor_demand;
		case 0:
			return adjusted_people.visitor_demand * tile->get_desc()->get_class_proportion(p_class) / class_proportions_sum;
		default:
			return adjusted_people.visitor_demand * tile->get_desc()->get_class_proportion(p_class) / class_proportions_sum - adjusted_people.visitor_demand * tile->get_desc()->get_class_proportion(p_class - 1) / class_proportions_sum;
	}
	return 0;
}

uint16 gebaeude_t::get_adjusted_jobs_by_class(uint8 p_class) const
{
	if (get_tile()->get_desc()->get_type() == building_desc_t::city_res) {
		return 0;
	}
	const uint8 pass_classes = goods_manager_t::passengers->get_number_of_classes();
	const uint32 class_proportions_sum = get_tile()->get_desc()->get_class_proportions_sum_jobs();
	if (class_proportions_sum == 0) {
		return adjusted_jobs / pass_classes;
	}
	if (p_class > pass_classes - 1) {
		return adjusted_jobs; // error
	}
	switch (p_class) {
	case -1:
		return adjusted_jobs;
	case 0:
		return adjusted_jobs * tile->get_desc()->get_class_proportion_jobs(p_class) / class_proportions_sum;
	default:
		return adjusted_jobs * tile->get_desc()->get_class_proportion_jobs(p_class) / class_proportions_sum - adjusted_jobs * tile->get_desc()->get_class_proportion_jobs(p_class - 1) / class_proportions_sum;
	}
	return 0;
}


sint32 gebaeude_t::check_remaining_available_jobs() const
{
	// Commenting out the "if(!jobs_available())" code will allow jobs to be shown as negative.
	/*if(!jobs_available())
	{
	// All the jobs are taken for the time being.
	//return 0;
	}
	else
	{*/
	if (available_jobs_by_time < welt->get_ticks() - welt->get_settings().get_job_replenishment_ticks())
	{
		// Uninitialised or stale - all jobs available
		return (sint64)adjusted_jobs;
	}
	const sint64 delta_t = welt->get_ticks() - available_jobs_by_time;
	const sint64 remaining_jobs = delta_t * (sint64)adjusted_jobs / welt->get_settings().get_job_replenishment_ticks();
	return (sint32)remaining_jobs;
	//}
}

sint32 gebaeude_t::get_staffing_level_percentage() const
{
	if (adjusted_jobs == 0)
	{
		return 100;
	}
	const sint32 percentage = (adjusted_jobs - check_remaining_available_jobs()) * 100 / adjusted_jobs;
	return percentage;
}

bool gebaeude_t::jobs_available() const
{
	const sint64 ticks = welt->get_ticks();
	bool difference = available_jobs_by_time <= ticks;
	return difference;
}

uint8 gebaeude_t::get_random_class(const goods_desc_t * wtyp)
{
	// This currently simply uses the building type's proportions.
	// TODO: Allow this to be modified when dynamic building occupation
	// is introduced with the (eventual) new town growth code.

	// At present, mail classes are handled rather badly.

	const uint8 number_of_classes = goods_manager_t::passengers->get_number_of_classes();

	if (number_of_classes == 1)
	{
		return 0;
	}

	const uint32 sum = get_tile()->get_desc()->get_class_proportions_sum();

	if (sum == 0 || wtyp != goods_manager_t::passengers)
	{
		// If the building has a zero sum of class proportions, as is the default, assume
		// an equal chance of any given class being generated from here.
		// Also, we don't have sensible figures to use for mail.
		return (uint8)simrand(wtyp->get_number_of_classes(), "uint8 gebaeude_t::get_random_class() const (fixed)");
	}

	const uint16 random = simrand(sum, "uint8 gebaeude_t::get_random_class() const (multiple classes)");

	uint8 g_class = 0;

	for (uint8 i = 0; i < number_of_classes; i++)
	{
		if (random < get_tile()->get_desc()->get_class_proportion(i))
		{
			g_class = i;
			break;
		}
	}

	return g_class;
}

void gebaeude_t::set_building_tiles()
{
	building_tiles.clear();
	const building_tile_desc_t* tile = get_tile();
	const building_desc_t *bdsc = tile->get_desc();
	const koord size = bdsc->get_size(tile->get_layout());
	if (size == koord(1, 1))
	{
		// A single tiled building - just add the single tile.
		building_tiles.append(welt->access_nocheck(get_pos().get_2d()));
	}
	else
	{
		// A multi-tiled building: check all tiles. Any tile within the
		// coverage radius of a building connects the whole building.

		// Then, store these tiles here, as this is computationally expensive
		// and frequently requested by the passenger/mail generation algorithm.

		koord3d k = get_pos();
		const koord start_pos = k.get_2d() - tile->get_offset();
		const koord end_pos = k.get_2d() + size;

		for (k.y = start_pos.y; k.y < end_pos.y; k.y++)
		{
			for (k.x = start_pos.x; k.x < end_pos.x; k.x++)
			{
				grund_t *gr = welt->lookup(k);
				if (gr)
				{
					/* This would fail for depots, but those are 1x1 buildings */
					gebaeude_t *gb_part = gr->find<gebaeude_t>();
					// There may be buildings with holes.
					if (gb_part && gb_part->get_tile()->get_desc() == bdsc)
					{
						const planquadrat_t* plan = welt->access_nocheck(k.get_2d());
						if (!plan->is_being_deleted())
						{
							building_tiles.append(plan);
						}
					}
				}
			}
		}
	}
}

void gebaeude_t::connect_by_road_to_nearest_city()
{
	if (get_stadt())
	{
		// Assume that this is already connected to a road if in a city.
		return;
	}
	koord3d start = get_pos();
	koord k = start.get_2d();
	grund_t* gr;
	bool start_found = false;
	for (uint8 i = 0; i < 8; i++)
	{
		// Check for connected roads. Only roads in immediately neighbouring tiles
		// and only those on the same height will register a connexion.
		start = koord3d(k + k.neighbours[i], get_pos().z);
		gr = welt->lookup(start);
		if (!gr)
		{
			continue;
		}
		if ((!gr->hat_wege() || gr->get_weg(road_wt)) && !gr->get_building() && !gr->is_water())
		{
			start_found = true;
			break;
		}
	}
	if (!start_found)
	{
		return;
	}

	// Next, find the nearest city
	const uint32 rank_max = welt->get_settings().get_auto_connect_industries_and_attractions_by_road();
	const uint32 max_road_length = env_t::networkmode ? 8192 : (uint32)env_t::intercity_road_length; // The env_t:: settings are not transmitted with network games so may diverge between client and server.
	for (uint32 rank = 1; rank <= rank_max; rank++)
	{
		const stadt_t* city = welt->find_nearest_city(get_pos().get_2d(), rank);
		if (!city)
		{
			return;
		}
		koord end = city->get_townhall_road();

		if (shortest_distance(start.get_2d(), end) > max_road_length)
		{
			return;
		}

		// Use the industry road type for attractions as well as industries
		way_desc_t const* desc = welt->get_settings().get_industry_road_type(welt->get_timeline_year_month());
		if (desc == NULL || !welt->get_settings().get_use_timeline())
		{
			// Hajo: try some default (might happen with timeline ... )
			desc = way_builder_t::weg_search(road_wt, 80, welt->get_timeline_year_month(), type_flat);
		}

		way_builder_t builder(NULL);
		builder.init_builder(way_builder_t::strasse | way_builder_t::terraform_flag, desc, tunnel_builder_t::get_tunnel_desc(road_wt, desc->get_topspeed(), welt->get_timeline_year_month()), bridge_builder_t::find_bridge(road_wt, desc->get_topspeed(), welt->get_timeline_year_month(), desc->get_max_axle_load() * 2));
		builder.set_keep_existing_ways(true);
		builder.set_maximum(max_road_length);
		builder.set_keep_city_roads(true);
		builder.set_build_sidewalk(false);
		builder.set_overtaking_mode(invalid_mode);
		builder.set_forbid_crossings(true); // Building crossings on industry roads can disrupt player railways.

		koord3d end3d = welt->lookup_kartenboden(end)->get_pos();

		builder.calc_route(end3d, start); // Start and end are inverted so as to produce cleaner routes: starting in the town and moving outwards means that the line of existing roads can be followed as far as possible.
		if (builder.get_count() > 1)
		{
			builder.build();
			break;
		}
	}
}
