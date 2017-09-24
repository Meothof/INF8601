/*
 * dragon_tbb.c
 *
 *  Created on: 2011-08-17
 *      Author: Francis Giraldeau <francis.giraldeau@gmail.com>
 */

#include <iostream>

extern "C" {
	#include "dragon.h"
	#include "color.h"
	#include "utils.h"
}
#include "dragon_tbb.h"
#include "tbb/tbb.h"
#include "TidMap.h"

using namespace std;
using namespace tbb;



class DragonLimits {
	private:
	piece_t piece;

	public:

	DragonLimits(){
		piece_init(&piece);
	}
	//Splitting constructor nécessaire au fonctionnement de parallel_reduce
	DragonLimits(const DragonLimits& dragon, split){
		piece_init(&piece);

	}
	//Accesseur de l'attribut piece
	piece_t getPiece();

	//operator qui accumule le resultat du calcul de piece_limit pour le morceau
	void operator()(const blocked_range<uint64_t> & r){
		piece_limit(r.begin(), r.end(), &piece);
	}
	//methode join permet de merge le morceau obtenu avec l'ensemble
	void join(DragonLimits& dragon){
		piece_merge(&piece, dragon.piece);
	}

};

//Accesseur de piece pour DragonLimits
piece_t DragonLimits::getPiece(){
	return piece;
}

class DragonDraw {
	private:
	//attributes
	struct draw_data* data;
	TidMap* tid_map;

	public:
	DragonDraw(struct draw_data* d, TidMap* tm){
		data = d;
		tid_map = tm;
	}
	DragonDraw(const DragonDraw& dragon){
		data = dragon.data;
		tid_map = dragon.tid_map;
	}
	void operator()(const blocked_range<uint64_t> & r) const{
		//version 1
		//			dragon_draw_raw(r.begin(), r.end(), data->dragon, data->dragon_width, data->dragon_height, data->limits, gettid());
		
		tid_map->getIdFromTid(gettid());
		uint64_t start = r.begin();
		int id_start = start * data->nb_thread / data->size;
		int id_end = id_start + 1;
		uint64_t end = id_end * data->size / data->nb_thread;
		while(true){
			id_end = id_start + 1;
			end = id_end * data->size / data->nb_thread;
			if(r.end() <= end){
				dragon_draw_raw(start, r.end(), data->dragon, data->dragon_width, data->dragon_height, data->limits, id_start);
				break;
			}
			else{
				dragon_draw_raw(start, end, data->dragon, data->dragon_width, data->dragon_height, data->limits, id_start);
				start = end;
				id_start++;
			}


		}


	}

};


class DragonRender {
	private:
	//attributes
	struct draw_data* data;

	public:
	//Constructeur initial
	DragonRender(struct draw_data* d){
		data = d;
	}
	//Copy constructor : fait des copies de l'objet pour les différents threads
	DragonRender(const DragonRender& dragon){
		data = dragon.data;
	}
	void operator()(const blocked_range<uint64_t> & r) const{
		scale_dragon(r.begin(), r.end(), data->image, data->image_width, data->image_height, data->dragon, data->dragon_width, data->dragon_height, data->palette );
	}

};

class DragonClear {
	private:
	//attributes
	char* canvas;

	public:
	//Constructeur initial
	DragonClear(char* c){
		canvas = c;
	}
	//Copy constructor : fait des copies de l'objet pour les différents threads
	DragonClear(const DragonClear& dragon){
		canvas = dragon.canvas;
	}

	void operator()(const blocked_range<uint64_t>& r) const{
		init_canvas(r.begin(), r.end(), canvas, -1);
	}

};

int dragon_draw_tbb(char **canvas, struct rgb *image, int width, int height, uint64_t size, int nb_thread)
{
	//	TODO("dragon_draw_tbb");
	struct draw_data data;
	limits_t limits;
	char *dragon = NULL;
	int dragon_width;
	int dragon_height;
	int dragon_surface;
	int scale_x;
	int scale_y;
	int scale;
	int deltaJ;
	int deltaI;

	struct palette *palette = init_palette(nb_thread);
	if (palette == NULL)
		return -1;

	/* 1. Calculer les limites du dragon */
	dragon_limits_tbb(&limits, size, nb_thread);

	task_scheduler_init init(nb_thread);

	dragon_width = limits.maximums.x - limits.minimums.x;
	dragon_height = limits.maximums.y - limits.minimums.y;
	dragon_surface = dragon_width * dragon_height;
	scale_x = dragon_width / width + 1;
	scale_y = dragon_height / height + 1;
	scale = (scale_x > scale_y ? scale_x : scale_y);
	deltaJ = (scale * width - dragon_width) / 2;
	deltaI = (scale * height - dragon_height) / 2;

	dragon = (char *) malloc(dragon_surface);
	if (dragon == NULL) {
		free_palette(palette);
		return -1;
	}

	data.nb_thread = nb_thread;
	data.dragon = dragon;
	data.image = image;
	data.size = size;
	data.image_height = height;
	data.image_width = width;
	data.dragon_width = dragon_width;
	data.dragon_height = dragon_height;
	data.limits = limits;
	data.scale = scale;
	data.deltaI = deltaI;
	data.deltaJ = deltaJ;
	data.palette = palette;
	data.tid = (int *) calloc(nb_thread, sizeof(int));
	TidMap tid_map(nb_thread);

	/* 2. Initialiser la surface : DragonClear */
	DragonClear dragonClear(dragon);
	parallel_for(blocked_range<uint64_t>(0,dragon_surface),dragonClear);

	/* 3. Dessiner le dragon : DragonDraw */
	DragonDraw dragonDraw(&data, &tid_map);
	parallel_for(blocked_range<uint64_t>(0,data.size), dragonDraw);
	/* 4. Effectuer le rendu final */
	DragonRender dragonRender(&data);
	parallel_for(blocked_range<uint64_t>(0,data.image_height), dragonRender);

	init.terminate();

	free_palette(palette);
	FREE(data.tid);
	*canvas = dragon;
	return 0;
}


/*
 * Calcule les limites en terme de largeur et de hauteur de
 * la forme du dragon. Requis pour allouer la matrice de dessin.
 */
int dragon_limits_tbb(limits_t *limits, uint64_t size, int nb_thread)
{
	//	TODO("dragon_limits_tbb");
	DragonLimits dragonLimits;
	task_scheduler_init task(nb_thread);
	parallel_reduce(blocked_range<uint64_t>(0,size), dragonLimits);
	*limits = dragonLimits.getPiece().limits;
	return 0;
}