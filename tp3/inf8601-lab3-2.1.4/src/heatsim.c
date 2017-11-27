/*
 * heatsim.c
 *
 *  Created on: 2011-11-17
 *      Author: francis
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <mpi.h>
#include <math.h>
#include <limits.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>

#include "config.h"
#include "part.h"
#include "grid.h"
#include "cart.h"
#include "image.h"
#include "heat.h"
#include "memory.h"
#include "util.h"

#define PROGNAME "heatsim"
#define DEFAULT_OUTPUT_PPM "heatsim.png"
#define DEFAULT_DIMX 1
#define DEFAULT_DIMY 1
#define DEFAULT_ITER 100
#define MAX_TEMP 1000.0
#define DIM_2D 2

typedef struct ctx {
    cart2d_t *cart;
    grid_t *global_grid;
    grid_t *curr_grid;
    grid_t *next_grid;
    grid_t *heat_grid;
    int numprocs;
    int rank;
    MPI_Comm comm2d;
    FILE *log;
    int verbose;
    int dims[DIM_2D];
    int isperiodic[DIM_2D];
    int coords[DIM_2D];
    int reorder;
    int north_peer;
    int south_peer;
    int east_peer;
    int west_peer;
    MPI_Datatype vector;
} ctx_t;

typedef struct command_opts {
    int dimx;
    int dimy;
    int iter;
    char *input;
    char *output;
    int verbose;
} opts_t;

static opts_t *global_opts = NULL;

__attribute__((noreturn))
    static void usage(void) {
    fprintf(stderr, PROGNAME " " VERSION " " PACKAGE_NAME "\n");
    fprintf(stderr, "Usage: " PROGNAME " [OPTIONS] [COMMAND]\n");
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --help	this help\n");
    fprintf(stderr, "  --iter	number of iterations to perform\n");
    fprintf(stderr, "  --dimx	2d decomposition in x dimension\n");
    fprintf(stderr, "  --dimy	2d decomposition in y dimension\n");
    fprintf(stderr, "  --input  png input file\n");
    fprintf(stderr, "  --output ppm output file\n");
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

static void dump_opts(struct command_opts *opts) {
    printf("%10s %s\n", "option", "value");
    printf("%10s %d\n", "dimx", opts->dimx);
    printf("%10s %d\n", "dimy", opts->dimy);
    printf("%10s %d\n", "iter", opts->iter);
    printf("%10s %s\n", "input", opts->input);
    printf("%10s %s\n", "output", opts->output);
    printf("%10s %d\n", "verbose", opts->verbose);
}

void default_int_value(int *val, int def) {
    if (*val == 0)
        *val = def;
}

static int parse_opts(int argc, char **argv, struct command_opts *opts) {
    int idx;
    int opt;
    int ret = 0;

    struct option options[] = { { "help", 0, 0, 'h' },
                               { "iter", 1, 0, 'r' }, { "dimx", 1, 0, 'x' }, { "dimy",
                                                                              1, 0, 'y' }, { "input", 1, 0, 'i' }, {
                                   "output", 1, 0, 'o' }, { "verbose", 0,
                                                           0, 'v' }, { 0, 0, 0, 0 } };

    memset(opts, 0, sizeof(struct command_opts));

    while ((opt = getopt_long(argc, argv, "hvx:y:l:", options, &idx)) != -1) {
        switch (opt) {
            case 'r':
                opts->iter = atoi(optarg);
                break;
            case 'y':
                opts->dimy = atoi(optarg);
                break;
            case 'x':
                opts->dimx = atoi(optarg);
                break;
            case 'i':
                if (asprintf(&opts->input, "%s", optarg) < 0)
                    goto err;
                break;
            case 'o':
                if (asprintf(&opts->output, "%s", optarg) < 0)
                    goto err;
                break;
            case 'h':
                usage();
                break;
            case 'v':
                opts->verbose = 1;
                break;
            default:
                printf("unknown option %c\n", opt);
                ret = -1;
                break;
        }
    }

    /* default values*/
    default_int_value(&opts->iter, DEFAULT_ITER);
    default_int_value(&opts->dimx, DEFAULT_DIMX);
    default_int_value(&opts->dimy, DEFAULT_DIMY);
    if (opts->output == NULL)
        if (asprintf(&opts->output, "%s", DEFAULT_OUTPUT_PPM) < 0)
            goto err;
    if (opts->input == NULL) {
        fprintf(stderr, "missing input file");
        goto err;
    }

    if (opts->dimx == 0 || opts->dimy == 0) {
        fprintf(stderr,
                "argument error: dimx and dimy must be greater than 0\n");
        ret = -1;
    }

    if (opts->verbose)
        dump_opts(opts);
    global_opts = opts;
    return ret;
    err:
    FREE(opts->input);
    FREE(opts->output);
    return -1;
}

FILE *open_logfile(int rank) {
    char str[255];
    sprintf(str, "out-%d", rank);
    FILE *f = fopen(str, "w+");
    return f;
}

ctx_t *make_ctx() {
    ctx_t *ctx = (ctx_t *) calloc(1, sizeof(ctx_t));
    return ctx;
}

void free_ctx(ctx_t *ctx) {
    if (ctx == NULL)
        return;
    free_grid(ctx->global_grid);
    free_grid(ctx->curr_grid);
    free_grid(ctx->next_grid);
    free_grid(ctx->heat_grid);
    free_cart2d(ctx->cart);
    if (ctx->log != NULL) {
        fflush(ctx->log);
        fclose(ctx->log);
    }
    FREE(ctx);
}

int init_ctx(ctx_t *ctx, opts_t *opts) {
//    TODO("lab3");


    MPI_Comm_size(MPI_COMM_WORLD, &ctx->numprocs);
    MPI_Comm_rank(MPI_COMM_WORLD, &ctx->rank);

    printf("rank %i enter init \n", ctx->rank);

    if (opts->dimx * opts->dimy != ctx->numprocs) {
        fprintf(stderr,
                "2D decomposition blocks must equal number of process\n");
        goto err;
    }
    ctx->log = open_logfile(ctx->rank);
    ctx->verbose = opts->verbose;
    ctx->dims[0] = opts->dimx;
    ctx->dims[1] = opts->dimy;
    ctx->isperiodic[0] = 1;
    ctx->isperiodic[1] = 1;
    ctx->reorder = 0;
    grid_t *new_grid = NULL;

    int r = MPI_SUCCESS;
    /* FIXME: create 2D cartesian communicator */
    r = MPI_Cart_create(MPI_COMM_WORLD, DIM_2D, ctx->dims, ctx->isperiodic, ctx->reorder, &ctx->comm2d);
    if(r != MPI_SUCCESS)
        goto err;

    r = MPI_Cart_shift(ctx->comm2d, 1, 1, &ctx->north_peer, &ctx->south_peer);
    if(r != MPI_SUCCESS)
        goto err;

    r = MPI_Cart_shift(ctx->comm2d, 0,1, &ctx->west_peer, &ctx->east_peer);
    if(r != MPI_SUCCESS)
        goto err;

    r = MPI_Cart_shift(ctx->comm2d, 1, -1, &ctx->south_peer, &ctx->north_peer);
    if(r != MPI_SUCCESS)
            goto err;

    r = MPI_Cart_shift(ctx->comm2d, 0,-1, &ctx->east_peer, &ctx->west_peer);
    if(r != MPI_SUCCESS)
        goto err;

    r = MPI_Cart_coords(ctx->comm2d, ctx->rank, DIM_2D, ctx->coords);
    if(r != MPI_SUCCESS)
        goto err;


    /*
	 * FIXME: le processus rank=0 charge l'image du disque
	 * et transfert chaque section aux autres processus
	 */

    MPI_Request *req;
    MPI_Status *status;


    if(ctx->rank == 0){
        /* load input image */
        image_t *image = load_png(opts->input);
        if (image == NULL)
            goto err;

        /* select the red channel as the heat source */
        ctx->global_grid = grid_from_image(image, CHAN_RED);

        /* grid is normalized to one, multiply by MAX_TEMP */
        grid_multiply(ctx->global_grid, MAX_TEMP);

        /* 2D decomposition */
        ctx->cart = make_cart2d(ctx->global_grid->width,
                                ctx->global_grid->height, opts->dimx, opts->dimy);
        cart2d_grid_split(ctx->cart, ctx->global_grid);

        /*
	 * FIXME: send grid dimensions and data
	 * Comment traiter le cas de rank=0 ?
	 */
        req = (MPI_Request *)malloc(4 * (ctx->numprocs -1)* sizeof(MPI_Request));
        status = (MPI_Status *)malloc(4 * (ctx->numprocs -1)* sizeof(MPI_Status));
        if(req == NULL){
            goto err;
        }
	    //Création d'un tableau qui contiendra les coordonnées d'un processus
        int coords[DIM_2D];
        int rank;
        for(rank = 1; rank < ctx->numprocs; rank++){
            //On récupère les coordonnées du processus rank
            MPI_Cart_coords(ctx->comm2d, rank, DIM_2D, coords);
            //On récupère le moreceau de grille correspondant
            grid_t *grid = cart2d_get_grid(ctx->cart, coords[0], coords[1]);

            //On envoie au noeud, les données nécéssaires à la création de sa grille.
            r = MPI_Isend(&grid->width, 1, MPI_INTEGER, rank, rank * 4 + 0, ctx->comm2d, &req[(rank-1) * 4]);
            if(r != MPI_SUCCESS)
                goto err;

            r = MPI_Isend(&grid->height, 1, MPI_INTEGER, rank, rank * 4 + 1, ctx->comm2d, &req[(rank-1) * 4 + 1]);
            if(r != MPI_SUCCESS)
                goto err;

            r = MPI_Isend(&grid->padding, 1 , MPI_INTEGER, rank, rank * 4 + 2, ctx->comm2d,&req[(rank-1) * 4] + 2);
            if(r != MPI_SUCCESS)
                            goto err;

            r = MPI_Isend(grid->dbl, grid->pw * grid->ph, MPI_DOUBLE, rank, rank * 4 + 3, ctx->comm2d, &req[(rank-1) * 4 + 3]);
            if(r != MPI_SUCCESS)
                goto err;


        }
        MPI_Waitall(4 * (ctx->numprocs -1), req, status);
        // Le noeud 0 crée sa propre grille
        MPI_Cart_coords(ctx->comm2d, ctx->rank, DIM_2D, coords);
        new_grid = grid_clone(cart2d_get_grid(ctx->cart, coords[0], coords[1]));
    }

    else{
        /*
        * FIXME: receive dimensions of the grid
        * store into new_grid
        */
        req = (MPI_Request *)malloc(4 * sizeof(MPI_Request));
        status = (MPI_Status *)malloc(4 * sizeof(MPI_Status));
        if(req == NULL || status == NULL)
            goto err;

        int width;
        int height;
        int padding;
        int rank = ctx->rank;
        //On récupère les données envoyées par le noeud 0.
        r = MPI_Irecv(&width, 1, MPI_INTEGER, 0, rank * 4 + 0 , ctx->comm2d, &req[0]);
        if(r != MPI_SUCCESS)
            goto err;

        r = MPI_Irecv(&height, 1, MPI_INTEGER, 0, rank * 4 + 1 , ctx->comm2d, &req[1]);
        if(r != MPI_SUCCESS)
            goto err;

        r = MPI_Irecv(&padding, 1, MPI_INTEGER, 0, rank * 4 + 2 , ctx->comm2d, &req[2]);
        if(r != MPI_SUCCESS)
            goto err;


        // On attend que le noeud ai recu width, height et padding
        MPI_Waitall(3, req, status);

        // On initialise la grille avec les arguments reçus
        new_grid = make_grid(width, height, padding);

        // On insère les données reçues dans la nouvelle grille
        r = MPI_Irecv(new_grid->dbl, new_grid->pw*new_grid->ph, MPI_DOUBLE, 0, rank * 4 + 3, ctx->comm2d, &req[3]);
        if(r != MPI_SUCCESS)
            goto err;

        MPI_Waitall(1, req + 3, status);
        // On libère le tableau status
        free(status);

    }

    // On libère le tableau req
    free(req);


    if (new_grid == NULL)
        goto err;
    /* set padding required for Runge-Kutta */
    ctx->curr_grid = grid_padding(new_grid, 1);
    ctx->next_grid = grid_padding(new_grid, 1);
    ctx->heat_grid = grid_padding(new_grid, 1);

    free_grid(new_grid);

    /* FIXME: create type vector to exchange columns */
    MPI_Type_vector(ctx->curr_grid->height, 1, ctx->curr_grid->pw, MPI_DOUBLE, &ctx->vector);
    MPI_Type_commit(&ctx->vector);

    printf("rank %i leave init \n", ctx->rank);
    return 0;
    err: return -1;
}

void dump_ctx(ctx_t *ctx) {
    fprintf(ctx->log, "*** CONTEXT ***\n");
    fprintf(ctx->log, "rank=%d\n", ctx->rank);
    fprintf(ctx->log, "north=%d south=%d west=%d east=%d \n",
            ctx->north_peer, ctx->south_peer,
            ctx->east_peer, ctx->west_peer);
    fprintf(ctx->log, "***************\n");
}

void exchng2d(ctx_t *ctx) {

//    printf("rank %i enter exchng2d \n", ctx->rank);

    grid_t *grid = ctx->curr_grid;
    int width = grid->pw;
    int height = grid->ph;
    double *data = grid->dbl;

    int dbl_width = grid->width;
    int padding = grid->padding;

    int north = ctx->north_peer;
    int south = ctx->south_peer;
    int west = ctx->west_peer;
    int east = ctx->east_peer;

    MPI_Comm comm = ctx->comm2d;
    MPI_Request *req = (MPI_Request *)malloc(8 * sizeof(MPI_Request));
    MPI_Status *status = (MPI_Status *)malloc(8 * sizeof(MPI_Status));



    //Calcul des offsets
    double *offset_send_north = data + (width + 1) * padding;
    double *offset_recv_north = offset_send_north - width;

    double *offset_send_south = data + width * height - padding * width - padding - dbl_width + 1;
    double *offset_recv_south = offset_send_south + width;

    double *offset_send_west = offset_send_north;
    double *offset_recv_west = offset_send_west - 1;

    double *offset_send_east = offset_send_north + dbl_width - 1;
    double *offset_recv_east = offset_send_east +1;

    //Attente de receptions non bloquantes des données
    MPI_Irecv(offset_recv_north, dbl_width, MPI_DOUBLE, north, 0, comm, &req[0]);
    MPI_Irecv(offset_recv_south, dbl_width, MPI_DOUBLE, south, 1, comm, &req[1]);
    MPI_Irecv(offset_recv_west, 1, ctx->vector, west, 2, comm, &req[2]);
    MPI_Irecv(offset_recv_east, 1, ctx->vector, east, 3, comm, &req[3]);

    //Envoie non bloquant des données
    MPI_Isend(offset_send_north, dbl_width, MPI_DOUBLE, north, 1, comm, &req[4]);
    MPI_Isend(offset_send_south, dbl_width, MPI_DOUBLE, south, 0, comm, &req[5]);
    MPI_Isend(offset_send_west, 1, ctx->vector, west, 3, comm, &req[6]);
    MPI_Isend(offset_send_east, 1, ctx->vector, east, 2, comm, &req[7]);

    MPI_Waitall(8, req, status);


    free(req);
    free(status);
//    printf("rank %i leave exchang2 \n", ctx->rank);



}

int gather_result(ctx_t *ctx, opts_t *opts) {

//    printf("rank %i enter gatherresult \n", ctx->rank);

    int ret = 0;
    int r = MPI_SUCCESS;
    grid_t *local_grid = grid_padding(ctx->next_grid, 0);


    MPI_Request *req = (MPI_Request *)malloc(ctx->numprocs * sizeof(MPI_Request));
    MPI_Status *status = (MPI_Status *)malloc(ctx->numprocs * sizeof(MPI_Status));

    if(local_grid == NULL || req == NULL || status == NULL){
        goto err;
    }
    //Le rang 0 doit copier lui même le contenu de local_grid et placer les grilles local des autres noeud dans sa grille cart
    if(ctx->rank == 0){
        int coords[DIM_2D];
        r = MPI_Cart_coords(ctx->comm2d, 0, DIM_2D, coords);
//        printf("MPI cart coord \n");
        if(r != MPI_SUCCESS){
            printf("Error MPI cart coord \n");
            goto err;
        }

        //Pour le rang 0, on copie le contenu de local grid dans la grille cart du noeud 0
        grid_copy(local_grid, cart2d_get_grid(ctx->cart, 0, 0));
        int rank;
        for(rank = 1; rank < ctx->numprocs; rank++){

            // On récupère les coordonnées cartésiennes du processus rank
            r = MPI_Cart_coords(ctx->comm2d, rank, DIM_2D, coords);
            if(r != MPI_SUCCESS){
                printf("Rank 0 : Error MPI cart coord on %i \n", rank);
                goto err;
            }

            //On place les données envoyées par le processus rank dans la grille cart du noeud 0
            r = MPI_Irecv(cart2d_get_grid(ctx->cart, coords[0], coords[1])->dbl, cart2d_get_grid(ctx->cart, coords[0], coords[1])->height * cart2d_get_grid(ctx->cart, coords[0], coords[1])->width, MPI_DOUBLE, rank, 4, ctx->comm2d, &req[rank - 1]);

            if(r != MPI_SUCCESS){
                printf("Rank 0 : Error MPI Irecv on %i \n", rank);
                goto err;
            }


        }
        //On attend l'arrivée de tous les messages
        MPI_Waitall(ctx->numprocs - 1, req, status);


    }
    else{
        //On envoie les données locales au processus 0
        r = MPI_Send(local_grid->dbl, local_grid->height * local_grid->width, MPI_DOUBLE, 0, 4, ctx->comm2d);
        if(r != MPI_SUCCESS)
            goto err;
    }


    //On merge les resultats dans la grille global
    cart2d_grid_merge(ctx->cart, ctx->global_grid);


    done:

        free_grid(local_grid);
        free(req);
        free(status);

        return ret;


    err: ret = -1;
        goto done;
}

int main(int argc, char **argv) {
    
    ctx_t *ctx = NULL;
    int rep, ret;
    opts_t opts;

    if (parse_opts(argc, argv, &opts) < 0) {
        printf("Error while parsing arguments\n");
        usage();
    }
    if (opts.verbose)
        dump_opts(&opts);

    MPI_Init(&argc, &argv);

    ctx = make_ctx();
    if (init_ctx(ctx, &opts) < 0)
        printf("Error in init");
        goto err;
    if (opts.verbose)
        dump_ctx(ctx);

    if (ctx->verbose) {
        fprintf(ctx->log, "heat grid\n");
        fdump_grid(ctx->heat_grid, ctx->log);
    }

    for (rep = 0; rep < opts.iter; rep++) {
        if (ctx->verbose) {
            fprintf(ctx->log, "iter %d\n", rep);
            fprintf(ctx->log, "start\n");
            fdump_grid(ctx->curr_grid, ctx->log);
        }

        grid_set_min(ctx->heat_grid, ctx->curr_grid);
        if (ctx->verbose) {
            fprintf(ctx->log, "grid_set_min\n");
            fdump_grid(ctx->curr_grid, ctx->log);
        }

        exchng2d(ctx);
        if (ctx->verbose) {
            fprintf(ctx->log, "exchng2d\n");
            fdump_grid(ctx->curr_grid, ctx->log);
        }

        heat_diffuse(ctx->curr_grid, ctx->next_grid);
        if (ctx->verbose) {
            fprintf(ctx->log, "heat_diffuse\n");
            fdump_grid(ctx->next_grid, ctx->log);
        }
        SWAP(ctx->curr_grid, ctx->next_grid);
    }

    MPI_Barrier(MPI_COMM_WORLD);
    if (gather_result(ctx, &opts) < 0)
        goto err;

    if (ctx->rank == 0) {
        printf("saving...\n");
        if (save_grid_png(ctx->global_grid, opts.output) < 0) {
            printf("saving failed\n");
            goto err;
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
    ret = EXIT_SUCCESS;
    done:
    free_ctx(ctx);
    MPI_Finalize();
    FREE(opts.input);
    FREE(opts.output);
    return ret;
    err:
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    ret = EXIT_FAILURE;
    goto done;
}

