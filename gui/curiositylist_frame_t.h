/*
 * This file is part of the Simutrans-Extended project under the Artistic License.
 * (see LICENSE.txt)
 */

#ifndef GUI_CURIOSITYLIST_FRAME_T_H
#define GUI_CURIOSITYLIST_FRAME_T_H


#include "gui_frame.h"
#include "components/action_listener.h"
#include "components/gui_combobox.h"
#include "components/gui_scrolled_list.h"
#include "components/gui_button.h"
#include "components/gui_combobox.h"


 /**
  * Curiosity list window
  * @author Hj. Malthaner
  */
class curiositylist_frame_t : public gui_frame_t, private action_listener_t
{
private:
	gui_combobox_t	sortedby;
	button_t sort_asc, sort_desc;
	button_t	filter_within_network;
	gui_scrolled_list_t scrolly;
	gui_aligned_container_t list;

	uint32 attraction_count;

	void fill_list();
public:
	curiositylist_frame_t();

	/**
	 * Set the window associated helptext
	 * @return the filename for the helptext, or NULL
	 * @author V. Meyer
	 */
	const char * get_help_filename() const OVERRIDE { return "curiositylist_filter.txt"; }

	bool action_triggered(gui_action_creator_t*, value_t) OVERRIDE;

	void draw(scr_coord pos, scr_size size) OVERRIDE;
};

#endif
