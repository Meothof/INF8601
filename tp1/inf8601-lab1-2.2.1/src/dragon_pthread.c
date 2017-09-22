/*
 * dragon_pthread.c
 *
 *  Created on: 2011-08-17
 *      Author: Francis Giraldeau <francis.giraldeau@gmail.com>
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>

#include "dragon.h"
#include "color.h"
#include "dragon_pthread.h"

pthread_mutex_t mutex_stdout;

void printf_threadsafe(char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	pthread_mutex_lock(&mutex_stdout);
	vprintf(format, ap);
	pthread_mutex_unlock(&mutex_stdout);
	va_end(ap);
}

void *dragon_draw_worker(void *data)
{
	
	
	// Conversion de la structure passée en pointeur
	struct draw_data *drw_data = (struct draw_data *) data;
	/* 1. Initialiser la surface */
	// On calcule la surface 
	long int dragon_surface = drw_data->dragon_width * drw_data->dragon_height;
	
	uint64_t start_canvas = drw_data->id*dragon_surface / (long int)drw_data->nb_thread;
	uint64_t end_canvas = (drw_data->id+1)*dragon_surface / (long int)drw_data->nb_thread;

	init_canvas(start_canvas, end_canvas, drw_data->dragon, -1);
	
	pthread_barrier_wait(drw_data->barrier);
	/* 2. Dessiner le dragon */
	uint64_t start = drw_data->id * drw_data->size / drw_data->nb_thread;
	uint64_t end = (drw_data->id + 1) * drw_data->size / drw_data->nb_thread;
	dragon_draw_raw(start, end, drw_data->dragon, drw_data->dragon_width, drw_data->dragon_height, drw_data->limits, drw_data->id);
	
	/* 3. Effectuer le rendu final */
	start = drw_data->id * drw_data->image_height /drw_data->nb_thread;
	end = (drw_data->id + 1) * drw_data->image_height / drw_data->nb_thread;
	scale_dragon(start, end, drw_data->image, drw_data->image_width, drw_data->image_height, drw_data->dragon, drw_data->dragon_width, drw_data->dragon_height, drw_data->palette);
	pthread_barrier_wait(drw_data->barrier);
	
	return NULL;
}

int dragon_draw_pthread(char **canvas, struct rgb *image, int width, int height, uint64_t size, int nb_thread)
{
//	TODO("dragon_draw_pthread");

	pthread_t *threads = NULL;
	pthread_barrier_t barrier;
	limits_t lim;
	struct draw_data info;
	char *dragon = NULL;
	int i;
	int scale_x;
	int scale_y;
	struct draw_data *data = NULL;
	struct palette *palette = NULL;
	int ret = 0;
	int r;

	palette = init_palette(nb_thread);
	if (palette == NULL)
		goto err;

	/* 1. Initialiser barrier. */

	r = pthread_barrier_init(&barrier, NULL, nb_thread);
	if(r != 0){
		printf("erreur initialisation de barrier");
		goto err;
	}
	if (dragon_limits_pthread(&lim, size, nb_thread) < 0)
		goto err;

	info.dragon_width = lim.maximums.x - lim.minimums.x;
	info.dragon_height = lim.maximums.y - lim.minimums.y;

	if ((dragon = (char *) malloc(info.dragon_width * info.dragon_height)) == NULL) {
//		printf("malloc error dragon\n");
		goto err;
	}

	if ((data = malloc(sizeof(struct draw_data) * nb_thread)) == NULL) {
//		printf("malloc error data\n");
		goto err;
	}

	if ((threads = malloc(sizeof(pthread_t) * nb_thread)) == NULL) {
//		printf("malloc error threads\n");
		goto err;
	}

	info.image_height = height;
	info.image_width = width;
	scale_x = info.dragon_width / width + 1;
	scale_y = info.dragon_height / height + 1;
	info.scale = (scale_x > scale_y ? scale_x : scale_y);
	info.deltaJ = (info.scale * width - info.dragon_width) / 2;
	info.deltaI = (info.scale * height - info.dragon_height) / 2;
	info.nb_thread = nb_thread;
	info.dragon = dragon;
	info.image = image;
	info.size = size;
	info.limits = lim;
	info.barrier = &barrier;
	info.palette = palette;
	info.dragon = dragon;
	info.image = image;
	
	
	/* 2. Lancement du calcul parallèle principal avec draw_dragon_worker */
	for (i = 0; i < nb_thread; i++) 
	{
		data[i] = info;
		data[i].id = i;
		if(pthread_create(&threads[i], NULL, dragon_draw_worker, &data[i])!=0)
			goto err;
	}
	/* 3. Attendre la fin du traitement */
	
	for(i = 0; i < nb_thread; i++)
	{
		pthread_join(threads[i],NULL);
	}

	/* 4. Destruction des variables (à compléter). */ 
	if (pthread_barrier_destroy(&barrier) != 0) {
		goto err;
	}

done:
	FREE(data);
	FREE(threads);
	free_palette(palette);
	*canvas = dragon;
	return ret;

err:
	FREE(dragon);
	ret = -1;
	goto done;
}

void *dragon_limit_worker(void *data)
{
	struct limit_data *lim = (struct limit_data *) data;
	piece_limit(lim->start, lim->end, &lim->piece);
	return NULL;
}

/*
 * Calcule les limites en terme de largeur et de hauteur de
 * la forme du dragon. Requis pour allouer la matrice de dessin.
 */
int dragon_limits_pthread(limits_t *limits, uint64_t size, int nb_thread)
{
//	TODO("dragon_limits_pthread");

	int ret = 0;
	int i;
	pthread_t *threads = NULL;
	struct limit_data *thread_data = NULL;
	piece_t master;

	piece_init(&master);

	/* 1. ALlouer de l'espace pour threads et threads_data. */

	
	threads = malloc(sizeof(pthread_t) * nb_thread);
	thread_data = malloc(sizeof(struct limit_data)* nb_thread);
	if(threads == NULL || thread_data == NULL){
		printf("Erreur d'allocation mémoire pour threads et/ou nb_thread");
		goto err;
	}
	
	/* 2. Lancement du calcul en parallèle avec dragon_limit_worker. */
	for(i =0 ; i<nb_thread; i++){
		thread_data[i].id = i;
		thread_data[i].start = i*size/nb_thread;
		thread_data[i].end = (i+1)*size/nb_thread;
		thread_data[i].piece = master;
		pthread_create(&threads[i],NULL,dragon_limit_worker,&thread_data[i]);
	}
	
	/* 3. Attendre la fin du traitement. */
	for(i = 0; i < nb_thread; i++)
	{
		pthread_join(threads[i],NULL);
	}
	/* Fusion des morceaux */
	for(i = 0; i < nb_thread; i++)
	{
		piece_merge(&master, thread_data[i].piece);
	}

done:
	FREE(threads);
	FREE(thread_data);
	*limits = master.limits;
	return ret;
err:
	ret = -1;
	goto done;
}