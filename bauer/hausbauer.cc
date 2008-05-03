/*
 * Copyright (c) 1997 - 2002 Hansj�rg Malthaner
 *
 * This file is part of the Simutrans project under the artistic licence.
 * (see licence.txt)
 */

#include <algorithm>
#include <string.h>

#include "../besch/haus_besch.h"
#include "../besch/skin_besch.h"
#include "../besch/spezial_obj_tpl.h"

#include "../boden/boden.h"
#include "../boden/fundament.h"

#include "../dataobj/translator.h"

#include "../dings/leitung2.h"
#include "../dings/zeiger.h"

#include "../gui/karte.h"
#include "../gui/werkzeug_waehler.h"

#include "../simdebug.h"
#include "../simdepot.h"
#include "../simhalt.h"
#include "../simskin.h"
#include "../simtools.h"
#include "../simwerkz.h"
#include "../simworld.h"
#include "../tpl/stringhashtable_tpl.h"
#include "../tpl/weighted_vector_tpl.h"
#include "../utils/simstring.h"
#include "hausbauer.h"

/*
 * Die verschiedenen Geb�udegruppen sind in eigenen Listen gesammelt.
 */
static vector_tpl<const haus_besch_t*> wohnhaeuser;
static vector_tpl<const haus_besch_t*> gewerbehaeuser;
static vector_tpl<const haus_besch_t*> industriehaeuser;
slist_tpl<const haus_besch_t *> hausbauer_t::sehenswuerdigkeiten_land;
slist_tpl<const haus_besch_t *> hausbauer_t::sehenswuerdigkeiten_city;
slist_tpl<const haus_besch_t *> hausbauer_t::rathaeuser;
slist_tpl<const haus_besch_t *> hausbauer_t::denkmaeler;
slist_tpl<const haus_besch_t *> hausbauer_t::ungebaute_denkmaeler;

/*
 * Diese Tabelle erm�glicht das Auffinden einer Beschreibung durch ihren Namen
 */
static stringhashtable_tpl<const haus_besch_t*> besch_names;

/*
 * Alle Geb�ude, die die Anwendung direkt ben�tigt, kriegen feste IDs.
 * Au�erdem m�ssen wir daf�r sorgen, dass sie alle da sind.
 */
const haus_besch_t *hausbauer_t::elevated_foundation_besch = NULL;

// all buildings with rails or connected to stops
vector_tpl<const haus_besch_t *> hausbauer_t::station_building;

vector_tpl<const haus_besch_t *> hausbauer_t::headquarter;

static spezial_obj_tpl<haus_besch_t> spezial_objekte[] = {
    { &hausbauer_t::elevated_foundation_besch,   "MonorailGround" },
    { NULL, NULL }
};


static bool compare_haus_besch(const haus_besch_t* a, const haus_besch_t* b)
{
	int diff = a->gib_level() - b->gib_level();
	if (diff == 0) {
		/* Gleiches Level - wir f�hren eine k�nstliche, aber eindeutige Sortierung
		 * �ber den Namen herbei. */
		diff = strcmp(a->gib_name(), b->gib_name());
	}
	return diff < 0;
}


static bool compare_station_besch(const haus_besch_t* a, const haus_besch_t* b)
{
	int diff = a->get_enabled() - b->get_enabled();
	if (diff == 0) {
		diff = a->gib_level() - b->gib_level();
	}
	if (diff == 0) {
		/* Gleiches Level - wir f�hren eine k�nstliche, aber eindeutige Sortierung
		 * �ber den Namen herbei. */
		diff = strcmp(a->gib_name(), b->gib_name());
	}
	return diff < 0;
}


bool hausbauer_t::alles_geladen()
{
	std::sort(wohnhaeuser.begin(),      wohnhaeuser.end(),      compare_haus_besch);
	std::sort(gewerbehaeuser.begin(),   gewerbehaeuser.end(),   compare_haus_besch);
	std::sort(industriehaeuser.begin(), industriehaeuser.end(), compare_haus_besch);
	std::sort(station_building.begin(), station_building.end(), compare_station_besch);
	std::sort(headquarter.begin(),      headquarter.end(),      compare_haus_besch);
	warne_ungeladene(spezial_objekte, 1);
	return true;
}


bool hausbauer_t::register_besch(const haus_besch_t *besch)
{
	::register_besch(spezial_objekte, besch);

	switch(besch->gib_typ()) {
		case gebaeude_t::wohnung:   wohnhaeuser.push_back(besch);      break;
		case gebaeude_t::industrie: industriehaeuser.push_back(besch); break;
		case gebaeude_t::gewerbe:   gewerbehaeuser.push_back(besch);   break;

		case gebaeude_t::unbekannt:
		switch (besch->gib_utyp()) {
			case haus_besch_t::denkmal:           denkmaeler.append(besch);               break;
			case haus_besch_t::attraction_land:   sehenswuerdigkeiten_land.append(besch); break;
			case haus_besch_t::firmensitz:        headquarter.append(besch,4);              break;
			case haus_besch_t::rathaus:           rathaeuser.append(besch);               break;
			case haus_besch_t::attraction_city:   sehenswuerdigkeiten_city.append(besch); break;

			case haus_besch_t::fabrik: break;

			case haus_besch_t::hafen:
			case haus_besch_t::hafen_geb:
			case haus_besch_t::depot:
			case haus_besch_t::generic_stop:
			case haus_besch_t::generic_extension:
				station_building.append(besch,16);
DBG_DEBUG("hausbauer_t::register_besch()","Infrastructure %s",besch->gib_name());
				break;

			case haus_besch_t::weitere:
				if(strcmp(besch->gib_name(),"MonorailGround")==0) {
					// foundation for elevated ways
					elevated_foundation_besch = besch;
					break;
				}
			default:
DBG_DEBUG("hausbauer_t::register_besch()","unknown subtype %i of %s: ignored",besch->gib_utyp(),besch->gib_name());
				return false;
		}
	}

	/* supply the tiles with a pointer back to the matchin description
	 * this is needed, since each building is build of seperate tiles,
	 * even if it is part of the same description (haus_besch_t)
	 */
	const int max_index = besch->gib_all_layouts()*besch->gib_groesse().x*besch->gib_groesse().y;
	for( int i=0;  i<max_index;  i++  ) {
		const_cast<haus_tile_besch_t *>(besch->gib_tile(i))->setze_besch(besch);
	}

	if(besch_names.get(besch->gib_name())) {
		dbg->fatal("hausbauer_t::register_Besch()", "building %s duplicated", besch->gib_name());
	}
	besch_names.put(besch->gib_name(), besch);
	return true;
}



// the tools must survice closing ...
static stringhashtable_tpl<wkz_station_t *> station_tool;
static stringhashtable_tpl<wkz_depot_t *> depot_tool;

// all these menus will need a waytype ...
void hausbauer_t::fill_menu(werkzeug_waehler_t* wzw, haus_besch_t::utyp utyp, waytype_t wt, const karte_t* welt)
{
	const uint16 time = welt->get_timeline_year_month();
DBG_DEBUG("hausbauer_t::fill_menu()","maximum %i",station_building.get_count());
	for(  vector_tpl<const haus_besch_t *>::const_iterator iter = station_building.begin(), end = station_building.end();  iter != end;  ++iter  ) {
		const haus_besch_t* besch = (*iter);
//		DBG_DEBUG("hausbauer_t::fill_menu()", "try to add %s (%p)", besch->gib_name(), besch);
		if(  besch->gib_utyp()==utyp  &&  besch->gib_cursor()->gib_bild_nr(1) != IMG_LEER  &&  besch->gib_extra()==(uint16)wt  ) {
			if(time==0  ||  (besch->get_intro_year_month()<=time  &&  besch->get_retire_year_month()>time)) {

				if(utyp==haus_besch_t::depot) {
					wkz_depot_t *wkz = depot_tool.get(besch->gib_name());
					if(wkz==NULL) {
						// not yet in hashtable
						wkz = new wkz_depot_t();
						wkz->icon = besch->gib_cursor()->gib_bild_nr(1),
						wkz->cursor = besch->gib_cursor()->gib_bild_nr(0),
						wkz->default_param = besch->gib_name();
						depot_tool.put(besch->gib_name(),wkz);
					}
					wzw->add_werkzeug( (werkzeug_t*)wkz );
				}
				else {
					wkz_station_t *wkz = station_tool.get(besch->gib_name());
					if(wkz==NULL) {
						// not yet in hashtable
						wkz = new wkz_station_t();
						wkz->icon = besch->gib_cursor()->gib_bild_nr(1),
						wkz->cursor = besch->gib_cursor()->gib_bild_nr(0),
						wkz->default_param = besch->gib_name();
						station_tool.put(besch->gib_name(),wkz);
					}
					wzw->add_werkzeug( (werkzeug_t*)wkz );
				}
			}
		}
	}
}


// new map => reset monument list to ensure every monument appears only once per map
void hausbauer_t::neue_karte()
{
	slist_iterator_tpl<const haus_besch_t *>  iter(denkmaeler);
	ungebaute_denkmaeler.clear();
	while(iter.next()) {
		ungebaute_denkmaeler.append(iter.get_current());
	}
}



void hausbauer_t::remove( karte_t *welt, spieler_t *sp, gebaeude_t *gb )
{
	const haus_tile_besch_t *tile  = gb->gib_tile();
	const haus_besch_t *hb = tile->gib_besch();

	// get startpos and size
	const koord3d pos = gb->gib_pos() - koord3d( tile->gib_offset(), 0 );
	koord size = tile->gib_besch()->gib_groesse( tile->gib_layout() );
	koord k;

	// then remove factory
	fabrik_t *fab = gb->get_fabrik();
	if(fab) {
		// first remove fabrik_t pointers
		for(k.y = 0; k.y < size.y; k.y ++) {
			for(k.x = 0; k.x < size.x; k.x ++) {
				grund_t *gr = welt->lookup(koord3d(k,0)+pos);
				gebaeude_t *gb_part = gr->find<gebaeude_t>();
				if(gb_part) {
					// there may be buildings with holes, so we only remove our or the hole!
					if(gb_part->gib_tile()->gib_besch()==hb) {
						gb_part->setze_fab( NULL );
						planquadrat_t *plan = welt->access( k+pos.gib_2d() );
						for( int i=plan->get_haltlist_count()-1;  i>=0;  i--  ) {
							halthandle_t halt = plan->get_haltlist()[i];
							halt->remove_fabriken( fab );
							plan->remove_from_haltlist( welt, halt );
						}
					}
				}
			}
		}
		// remove all transformers
		for(k.y = pos.y; k.y < pos.y+size.y;  k.y ++) {
			k.x = pos.x-1;
			grund_t *gr = welt->lookup_kartenboden(k);
			if(gr) {
				senke_t *sk = gr->find<senke_t>();
				if(sk) delete sk;
				pumpe_t *pp = gr->find<pumpe_t>();
				if(pp) delete pp;
			}
			k.x = pos.x+size.x;
			gr = welt->lookup_kartenboden(k);
			if(gr) {
				senke_t *sk = gr->find<senke_t>();
				if(sk) delete sk;
				pumpe_t *pp = gr->find<pumpe_t>();
				if(pp) delete pp;
			}
		}
		for(k.x = pos.x; k.x < pos.x+size.x;  k.x ++) {
			k.y = pos.y-1;
			grund_t *gr = welt->lookup_kartenboden(k);
			if(gr) {
				senke_t *sk = gr->find<senke_t>();
				if(sk) delete sk;
				pumpe_t *pp = gr->find<pumpe_t>();
				if(pp) delete pp;
			}
			k.y = pos.y+size.y;
			gr = welt->lookup_kartenboden(k);
			if(gr) {
				senke_t *sk = gr->find<senke_t>();
				if(sk) delete sk;
				pumpe_t *pp = gr->find<pumpe_t>();
				if(pp) delete pp;
			}
		}
		// end clean up transformers
		welt->rem_fab(fab);
	}

	// delete just our house
	for(k.y = 0; k.y < size.y; k.y ++) {
		for(k.x = 0; k.x < size.x; k.x ++) {
			grund_t *gr = welt->lookup(koord3d(k,0)+pos);
			if(gr) {
				gebaeude_t *gb_part = gr->find<gebaeude_t>();
				// there may be buildings with holes, so we only remove our!
				if(gb_part  &&  gb_part->gib_tile()->gib_besch()==hb) {
					// ok, now we can go on with deletion
					gb_part->entferne( sp );
					delete gb_part;
					// if this was a station building: delete ground
					if(gr->gib_halt().is_bound()) {
						gr->gib_halt()->rem_grund(gr);
					}
					// and maybe restore land below
					if(gr->gib_typ()==grund_t::fundament) {
						const koord newk = k+pos.gib_2d();
						const uint8 new_slope = gr->gib_hoehe()==welt->min_hgt(newk) ? 0 : welt->calc_natural_slope(newk);
						if(welt->lookup(koord3d(newk,welt->min_hgt(newk)))!=gr) {
							// there is another ground below => do not change hight, keep foundation
							welt->access(newk)->kartenboden_setzen( new boden_t(welt, gr->gib_pos(), hang_t::flach ) );
						}
						else {
							welt->access(newk)->kartenboden_setzen(new boden_t(welt, koord3d(newk,welt->min_hgt(newk) ), new_slope) );
						}
						// there might be walls from foundations left => thus some tiles may needs to be redraw
						if(new_slope!=0) {
							if(pos.x<welt->gib_groesse_x()-1)
								welt->lookup_kartenboden(newk+koord::ost)->calc_bild();
							if(pos.y<welt->gib_groesse_y()-1)
								welt->lookup_kartenboden(newk+koord::sued)->calc_bild();
						}
					}
				}
			}
		}
	}
}



gebaeude_t* hausbauer_t::baue(karte_t* welt, spieler_t* sp, koord3d pos, int org_layout, const haus_besch_t* besch, void* param)
{
	gebaeude_t* first_building = NULL;
	koord k;
	koord dim;

	uint8 layout = besch->layout_anpassen(org_layout);
	dim = besch->gib_groesse(org_layout);
	bool needs_ground_recalc = false;

	for(k.y = 0; k.y < dim.y; k.y ++) {
		for(k.x = 0; k.x < dim.x; k.x ++) {
//DBG_DEBUG("hausbauer_t::baue()","get_tile() at %i,%i",k.x,k.y);
			const haus_tile_besch_t *tile = besch->gib_tile(layout, k.x, k.y);
			// here test for good tile
			if (tile == NULL || (
						k != koord(0, 0) &&
						besch->gib_utyp() != haus_besch_t::hafen &&
						tile->gib_hintergrund(0, 0, 0) == IMG_LEER &&
						tile->gib_vordergrund(0, 0)    == IMG_LEER
					)) {
						// may have a rotation, that is not recoverable
						DBG_MESSAGE("hausbauer_t::baue()","get_tile() empty at %i,%i",k.x,k.y);
				continue;
			}
			gebaeude_t *gb = new gebaeude_t(welt, pos + k, sp, tile);
			if (first_building == NULL) {
				first_building = gb;
			}

			if(besch->ist_fabrik()) {
//DBG_DEBUG("hausbauer_t::baue()","setze_fab() at %i,%i",k.x,k.y);
				gb->setze_fab((fabrik_t *)param);
			}
			// try to fake old building
			else if(welt->gib_zeit_ms() < 2) {
				// Hajo: after staring a new map, build fake old buildings
				gb->add_alter(10000);
			}
			grund_t *gr = welt->lookup(pos.gib_2d() + k)->gib_kartenboden();
			if(gr->ist_wasser()) {
				gr->obj_add(gb);
			} else if (besch->gib_utyp() == haus_besch_t::hafen) {
				// its a dock!
				gr->obj_add(gb);
			} else {
				// very likely remove all
				leitung_t *lt = NULL;
				if(!gr->hat_wege()) {
					lt = gr->find<leitung_t>();
					if(lt) {
						gr->obj_remove(lt);
					}
					gr->obj_loesche_alle(sp);	// alles weg au�er vehikel ...
				}
				needs_ground_recalc |= gr->gib_grund_hang()!=hang_t::flach;
				grund_t *gr2 = new fundament_t(welt, gr->gib_pos(), gr->gib_grund_hang());
				welt->access(gr->gib_pos().gib_2d())->boden_ersetzen(gr, gr2);
				gr = gr2;
//DBG_DEBUG("hausbauer_t::baue()","ground count now %i",gr->obj_count());
				gr->obj_add( gb );
				gb->setze_pos( gr->gib_pos() );
				if(lt) {
					gr->obj_add( lt );
				}
				if(needs_ground_recalc  &&  welt->ist_in_kartengrenzen(pos.gib_2d()+koord(1,1))  &&  (k.y+1==dim.y  ||  k.x+1==dim.x)) {
					welt->lookup(pos.gib_2d()+k+koord(1,0))->gib_kartenboden()->calc_bild();
					welt->lookup(pos.gib_2d()+k+koord(0,1))->gib_kartenboden()->calc_bild();
					welt->lookup(pos.gib_2d()+k+koord(1,1))->gib_kartenboden()->calc_bild();
				}
			}
			if(besch->ist_ausflugsziel()) {
				welt->add_ausflugsziel( gb );
			}
			if(besch->gib_typ() == gebaeude_t::unbekannt) {
				if(station_building.is_contained(besch)) {
					(*static_cast<halthandle_t *>(param))->add_grund(gr);
				}
				if (besch->gib_utyp() == haus_besch_t::hafen) {
					// its a dock!
					gb->setze_yoff(0);
				}
			}
			gr->calc_bild();
			reliefkarte_t::gib_karte()->calc_map_pixel(gr->gib_pos().gib_2d());
		}
		if(besch->gib_utyp()==haus_besch_t::denkmal) {
			ungebaute_denkmaeler.remove( besch );
		}
	}
	return first_building;
}



gebaeude_t *
hausbauer_t::neues_gebaeude(karte_t *welt, spieler_t *sp, koord3d pos, int built_layout, const haus_besch_t *besch, void *param)
{
	gebaeude_t *gb;

	uint8 corner_layout = 6;	// assume single building (for more than 4 layouts)

	// adjust layout of neighbouring building
	if(besch->gib_utyp()>=8  &&  besch->gib_all_layouts()>1) {

		int layout = built_layout & 9;

		// detect if we are connected at far (north/west) end
		sint8 offset = welt->lookup( pos )->gib_weg_yoff()/TILE_HEIGHT_STEP;
		grund_t * gr = welt->lookup( pos+koord3d( (layout & 1 ? koord::ost : koord::sued), offset) );
		if(!gr) {
			// check whether bridge end tile
			grund_t * gr_tmp = welt->lookup( pos+koord3d( (layout & 1 ? koord::ost : koord::sued),offset - 1) );
			if(gr_tmp && gr_tmp->gib_weg_yoff()/TILE_HEIGHT_STEP == 1) {
				gr = gr_tmp;
			}
		}
		if(gr) {
			gebaeude_t* gb = gr->find<gebaeude_t>();
			if(gb  &&  gb->gib_tile()->gib_besch()->gib_utyp()>=8) {
				corner_layout &= ~2; // clear near bit
				if(gb->gib_tile()->gib_besch()->gib_all_layouts()>4) {
					koord xy = gb->gib_tile()->gib_offset();
					uint8 layoutbase = gb->gib_tile()->gib_layout();
					if((layoutbase & 1) == (layout & 1)) {
						layoutbase &= 0xb; // clear near bit on neighbour
						gb->setze_tile(gb->gib_tile()->gib_besch()->gib_tile(layoutbase, xy.x, xy.y));
					}
				}
			}
		}

		// detect if near (south/east) end
		gr = welt->lookup( pos+koord3d( (layout & 1 ? koord::west : koord::nord), offset) );
		if(!gr) {
			// check whether bridge end tile
			grund_t * gr_tmp = welt->lookup( pos+koord3d( (layout & 1 ? koord::west : koord::nord),offset - 1) );
			if(gr_tmp && gr_tmp->gib_weg_yoff()/TILE_HEIGHT_STEP == 1) {
				gr = gr_tmp;
			}
		}
		if(gr) {
			gebaeude_t* gb = gr->find<gebaeude_t>();
			if(gb  &&  gb->gib_tile()->gib_besch()->gib_utyp()>=8) {
				corner_layout &= ~4; // clear far bit
				if(gb->gib_tile()->gib_besch()->gib_all_layouts()>4) {
					koord xy = gb->gib_tile()->gib_offset();
					uint8 layoutbase = gb->gib_tile()->gib_layout();
					if((layoutbase & 1) == (layout & 1)) {
						layoutbase &= 0xd; // clear far bit on neighbour
						gb->setze_tile(gb->gib_tile()->gib_besch()->gib_tile(layoutbase, xy.x, xy.y));
					}
				}
			}
		}
	}

	// adjust layouts of the new building
	if(besch->gib_all_layouts()>4) {
		built_layout = (corner_layout | (built_layout&9) ) % besch->gib_all_layouts();
	}

	const haus_tile_besch_t *tile = besch->gib_tile(built_layout, 0, 0);
	if(  besch->gib_utyp() == haus_besch_t::depot  ) {
		switch(  besch->gib_extra()  ) {
			case track_wt:
				gb = new bahndepot_t(welt, pos, sp, tile);
				break;
			case tram_wt:
				gb = new tramdepot_t(welt, pos, sp, tile);
				break;
			case monorail_wt:
				gb = new monoraildepot_t(welt, pos, sp, tile);
				break;
			case maglev_wt:
			case narrowgauge_wt:
				assert(0);
				break;
			case road_wt:
				gb = new strassendepot_t(welt, pos, sp, tile);
				break;
			case water_wt:
				gb = new schiffdepot_t(welt, pos, sp, tile);
				break;
			case air_wt:
				gb = new airdepot_t(welt, pos, sp, tile);
				break;
		}
	}
	else {
		gb = new gebaeude_t(welt, pos, sp, tile);
	}
//DBG_MESSAGE("hausbauer_t::neues_gebaeude()","building stop pri=%i",pri);

	// remove pointer
	grund_t *gr = welt->lookup(pos);
	zeiger_t* zeiger = gr->find<zeiger_t>();
	if (zeiger) gr->obj_remove(zeiger);

	gr->obj_add(gb);

	if(station_building.is_contained(besch)  &&  besch->gib_utyp()!=haus_besch_t::depot) {
		// is a station/bus stop
		(*static_cast<halthandle_t *>(param))->add_grund(gr);
		gr->calc_bild();
	}

	if(besch->ist_ausflugsziel()) {
		welt->add_ausflugsziel( gb );
	}
	reliefkarte_t::gib_karte()->calc_map_pixel(gb->gib_pos().gib_2d());

	return gb;
}



const haus_tile_besch_t *hausbauer_t::find_tile(const char *name, int org_idx)
{
	int idx = org_idx;
	const haus_besch_t *besch = besch_names.get(name);
	if(besch) {
		const int size = besch->gib_h()*besch->gib_b();
		if(  idx >= besch->gib_all_layouts()*size  ) {
			idx %= besch->gib_all_layouts()*size;
			DBG_MESSAGE("gebaeude_t::rdwr()","%s using tile %i instead of %i",name,idx,org_idx);
		}
		return besch->gib_tile(idx);
	}
//	DBG_MESSAGE("hausbauer_t::find_tile()","\"%s\" not in hashtable",name);
	return NULL;
}



const haus_besch_t* hausbauer_t::gib_random_station(const haus_besch_t::utyp utype, const waytype_t wt, const uint16 time, const uint8 enables)
{
	vector_tpl<const haus_besch_t*> stops;

	for(  vector_tpl<const haus_besch_t *>::const_iterator iter = station_building.begin(), end = station_building.end();  iter != end;  ++iter  ) {
		const haus_besch_t* besch = (*iter);
		if(besch->gib_utyp()==utype  &&  besch->gib_extra()==wt  &&  (enables==0  ||  (besch->get_enabled()&enables)!=0)) {
			// ok, now check timeline
			if(time==0  ||  (besch->get_intro_year_month()<=time  &&  besch->get_retire_year_month()>time)) {
				stops.push_back(besch);
			}
		}
	}
	return stops.empty() ? NULL : stops[simrand(stops.get_count())];
}



const haus_besch_t* hausbauer_t::gib_special(int bev, haus_besch_t::utyp utype, uint16 time, climate cl)
{
	weighted_vector_tpl<const haus_besch_t *> auswahl(16);
	slist_iterator_tpl<const haus_besch_t*> iter(utype == haus_besch_t::rathaus ? rathaeuser : (bev == -1 ? sehenswuerdigkeiten_land : sehenswuerdigkeiten_city));

	while(iter.next()) {
		const haus_besch_t *besch = iter.get_current();
		// extra data contains number of inhabitants for building
		if(bev == -1 || besch->gib_extra()==bev) {
			if(cl==MAX_CLIMATES  ||  besch->is_allowed_climate(cl)) {
				// ok, now check timeline
				if(time==0  ||  (besch->get_intro_year_month()<=time  &&  besch->get_retire_year_month()>time)) {
					auswahl.append(besch,besch->gib_chance(),4);
				}
			}
		}
	}
	if (auswahl.empty()) {
		return 0;
	}
	else	if(auswahl.get_count()==1) {
		return auswahl.front();
	}
	// now there is something to choose
	return auswahl.at_weight( simrand(auswahl.get_sum_weight()) );
}



/**
 * tries to find something that matches this entry
 * it will skip and jump, and will never return zero, if there is at least a single valid entry in the list
 * @author Hj. Malthaner
 */
static const haus_besch_t* gib_aus_liste(const vector_tpl<const haus_besch_t*>& liste, int level, uint16 time, climate cl)
{
	weighted_vector_tpl<const haus_besch_t *> auswahl(16);

//	DBG_MESSAGE("hausbauer_t::gib_aus_liste()","target level %i", level );
	const haus_besch_t *besch_at_least=NULL;
	for (vector_tpl<const haus_besch_t*>::const_iterator i = liste.begin(), end = liste.end(); i != end; ++i) {
		const haus_besch_t* besch = *i;
		if(	besch->is_allowed_climate(cl)  &&
			besch->gib_chance()>0  &&
			(time==0  ||  (besch->get_intro_year_month()<=time  &&  besch->get_retire_year_month()>time))) {
			besch_at_least = besch;
		}

		const int thislevel = besch->gib_level();
		if(thislevel>level) {
			if (auswahl.empty()) {
				// continue with search ...
				level = thislevel;
			}
			else {
				// ok, we found something
				break;
			}
		}

		if(thislevel==level  &&  besch->gib_chance()>0) {
			if(cl==MAX_CLIMATES  ||  besch->is_allowed_climate(cl)) {
				if(time==0  ||  (besch->get_intro_year_month()<=time  &&  besch->get_retire_year_month()>time)) {
//				DBG_MESSAGE("hausbauer_t::gib_aus_liste()","appended %s at %i", besch->gib_name(), thislevel );
					auswahl.append(besch,besch->gib_chance(),4);
				}
			}
		}
	}

	if(auswahl.get_sum_weight()==0) {
		// this is some level below, but at least it is something
		return besch_at_least;
	}
	if(auswahl.get_count()==1) {
		return auswahl.front();
	}
	// now there is something to choose
	return auswahl.at_weight( simrand(auswahl.get_sum_weight()) );
}


const haus_besch_t* hausbauer_t::gib_gewerbe(int level, uint16 time, climate cl)
{
	return gib_aus_liste(gewerbehaeuser, level, time, cl);
}


const haus_besch_t* hausbauer_t::gib_industrie(int level, uint16 time, climate cl)
{
	return gib_aus_liste(industriehaeuser, level, time, cl);
}


const haus_besch_t* hausbauer_t::gib_wohnhaus(int level, uint16 time, climate cl)
{
	return gib_aus_liste(wohnhaeuser, level, time, cl);
}


// get a random object
const haus_besch_t *hausbauer_t::waehle_aus_liste(slist_tpl<const haus_besch_t *> &liste, uint16 time, climate cl)
{
	if (!liste.empty()) {
		// previously just returned a random object; however, now we do als look at the chance entry
		weighted_vector_tpl<const haus_besch_t *> auswahl(16);
		slist_iterator_tpl <const haus_besch_t *> iter (liste);

		while(iter.next()) {
			const haus_besch_t *besch = iter.get_current();
			if((cl==MAX_CLIMATES  ||  besch->is_allowed_climate(cl))  &&  besch->gib_chance()>0  &&  (time==0  ||  (besch->get_intro_year_month()<=time  &&  besch->get_retire_year_month()>time))) {
//				DBG_MESSAGE("hausbauer_t::gib_aus_liste()","appended %s at %i", besch->gib_name(), thislevel );
				auswahl.append(besch,besch->gib_chance(),4);
			}
		}
		// now look, what we have got ...
		if(auswahl.get_sum_weight()==0) {
			return NULL;
		}
		if(auswahl.get_count()==1) {
			return auswahl.front();
		}
		// now there is something to choose
		return auswahl.at_weight( simrand(auswahl.get_sum_weight()) );
	}
	return NULL;
}



const slist_tpl<const haus_besch_t*>* hausbauer_t::get_list(const haus_besch_t::utyp typ)
{
	switch (typ) {
		case haus_besch_t::denkmal:         return &ungebaute_denkmaeler;
		case haus_besch_t::attraction_land: return &sehenswuerdigkeiten_land;
		case haus_besch_t::firmensitz:      return NULL;
		case haus_besch_t::rathaus:         return &rathaeuser;
		case haus_besch_t::attraction_city: return &sehenswuerdigkeiten_city;
		case haus_besch_t::fabrik:          return NULL;
		default:                            return NULL;
	}
}



const vector_tpl<const haus_besch_t*>* hausbauer_t::get_citybuilding_list(const gebaeude_t::typ typ)
{
	switch (typ) {
		case gebaeude_t::wohnung:   return &wohnhaeuser;
		case gebaeude_t::gewerbe:   return &gewerbehaeuser;
		case gebaeude_t::industrie: return &industriehaeuser;
		default:                    return NULL;
	}
}
