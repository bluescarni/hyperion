#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <ios>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "voro++/voro++.hh"

extern "C" const char * hyperion_voropp_wrap(int **neighbours, int start, int end, int *max_nn, double **volumes, double **bb_min, double **bb_max, double **vertices,
                                             int *max_nv, double xmin, double xmax, double ymin, double ymax, double zmin, double zmax,
                                             double const *points, int npoints, int with_vertices, const char *wall_str, const double *wall_args_arr, int n_wall_args, int verbose);

using namespace voro;

// Number of average particles per block for good performance,
// determined experimentally.
static const double particle_block = 5.;

// Simple smart pointer that uses std::free to deallocate.
template <typename T>
class ptr_raii
{
    public:
        explicit ptr_raii(T *ptr):m_ptr(ptr) {}
        ~ptr_raii()
        {
            if (m_ptr) {
                std::free(m_ptr);
            }
        }
        T *release()
        {
            T *retval = m_ptr;
            m_ptr = 0;
            return retval;
        }
        T *get()
        {
            return m_ptr;
        }
    private:
        // Remove copy ctor and assignment operator.
        ptr_raii(const ptr_raii &);
        ptr_raii &operator=(const ptr_raii &);
    private:
        T *m_ptr;
};

// Functor to extract the max number of neighbours/vertices.
template <typename T>
static inline bool size_cmp(const std::vector<T> &a, const std::vector<T> &b)
{
    return a.size() < b.size();
}

// Global string used for reporting errors back to Python.
static std::string error_message;

// Wall utilities.
// Need to store this statically as the wall object needs to exist outside the scope
// in which it is created.
static std::auto_ptr<wall> wall_ptr;

// Helper function to add the wall.
static inline void add_walls(container &con,const char *wall_str,const double *wall_args_arr,int n_wall_args,int verbose)
{
    if (verbose) {
        std::cout << "Wall type: " << wall_str << '\n';
        std::cout << "Wall number of args: " << n_wall_args << '\n';
        std::cout << "Wall params: [";
        for (int i = 0; i < n_wall_args; ++i) {
            std::cout << wall_args_arr[i];
            if (i != n_wall_args - 1) {
                std::cout << ',';
            }
        }
        std::cout << "]\n";
    }

    // Allowed walls: 'sphere','cylinder','cone','plane'.
    if (std::strcmp(wall_str,"sphere") == 0) {
        // Some checks.
        if (n_wall_args != 4) {
            throw std::invalid_argument("invalid number of arguments for a 'sphere' wall, exactly 4 are needed");
        }
        if (wall_args_arr[3] <= 0.) {
            throw std::invalid_argument("the radius of a 'sphere' wall must be strictly positive");
        }
        wall_ptr.reset(new wall_sphere(wall_args_arr[0],wall_args_arr[1],wall_args_arr[2],wall_args_arr[3]));
        con.add_wall(wall_ptr.get());
    }
    if (std::strcmp(wall_str,"cylinder") == 0) {
        // Some checks.
        if (n_wall_args != 7) {
            throw std::invalid_argument("invalid number of arguments for a 'cylinder' wall, exactly 7 are needed");
        }
        if (wall_args_arr[6] <= 0.) {
            throw std::invalid_argument("the radius of a 'cylinder' wall must be strictly positive");
        }
        wall_ptr.reset(new wall_cylinder(wall_args_arr[0],wall_args_arr[1],wall_args_arr[2],wall_args_arr[3],wall_args_arr[4],
            wall_args_arr[5],wall_args_arr[6]
        ));
        con.add_wall(wall_ptr.get());
    }
}

// Main wrapper called from cpython.
const char *hyperion_voropp_wrap(int **neighbours, int start, int end, int *max_nn, double **volumes, double **bb_min, double **bb_max, double **vertices,
                                 int *max_nv, double xmin, double xmax, double ymin, double ymax, double zmin, double zmax, double const *points,
                                 int nsites, int with_vertices, const char *wall_str, const double *wall_args_arr, int n_wall_args, int verbose)
{
    // We need to wrap everything in a try/catch block as exceptions cannot leak out to C.
    try {

    // Total number of blocks we want.
    const double nblocks = nsites / particle_block;

    // Average block edge.
    const double block_edge = cbrt(nblocks);

    // Average edge length of the domain.
    const double vol_edge = cbrt((xmax - xmin) * (ymax - ymin) * (zmax - zmin));

    // The number of grid blocks across each coordinate will be proportional
    // to the dimension of the domain in that coordinate. The +1 is to account for rounding
    // and to make sure that we always have at least 1 block.
    const int nx = (int)((xmax - xmin) / vol_edge * block_edge) + 1;
    const int ny = (int)((ymax - ymin) / vol_edge * block_edge) + 1;
    const int nz = (int)((zmax - zmin) / vol_edge * block_edge) + 1;

    // Number of cells to be computed
    const int ncells = end - start;

    if (verbose) {
        std::cout << "Total number of sites: " << nsites << '\n';
        std::cout << "Number of cells to be computed: " << ncells << '\n';
        std::cout << "Range: [" << start << ',' << end << "]\n";
        std::cout << "Domain: [" << xmin << ',' << xmax << "] [" << ymin << ',' << ymax << "] [" << zmin << ',' << zmax << "]\n";
        std::cout << "Initialising with the following block grid: " << nx << ',' << ny << ',' << nz << '\n';
        std::cout << std::boolalpha;
        std::cout << "Vertices: " << bool(with_vertices) << '\n';
    }

    // Prepare the output quantities.
    // Neighbour list.
    std::vector<std::vector<int> > n_list(ncells);
    // List of vertices.
    std::vector<std::vector<double> > vertices_list;
    if (with_vertices) {
        vertices_list.resize(ncells);
    }
    // Volumes.
    ptr_raii<double> vols(static_cast<double *>(std::malloc(sizeof(double) * ncells)));
    // Bounding boxes.
    ptr_raii<double> bb_m(static_cast<double *>(std::malloc(sizeof(double) * ncells * 3)));
    ptr_raii<double> bb_M(static_cast<double *>(std::malloc(sizeof(double) * ncells * 3)));

    // Initialise the voro++ container. All particles must be placed inside the container,
    // and we record in the po variable those particles whose cells we need to compute.
    particle_order po;
    container con(xmin,xmax,ymin,ymax,zmin,zmax,nx,ny,nz,
                  false,false,false,8);
    for(int i = 0; i < nsites; ++i) {
            if (i >= start && i < end) {
                con.put(po,i,points[i*3],points[i*3 + 1],points[i*3 + 2]);
            } else {
                con.put(i,points[i*3],points[i*3 + 1],points[i*3 + 2]);
            }
    }

    // Handle the walls.
    add_walls(con,wall_str,wall_args_arr,n_wall_args,verbose);

    // Initialise the looping variables and the temporary cell object used for computation.
    voronoicell_neighbor c;
    c_loop_order vl(con,po);
    int idx;
    double tmp_min[3],tmp_max[3];
    std::vector<double> tmp_v;
    // Site position and radius (r is unused).
    double x,y,z,r;

    // Loop over the selected particles and compute the desired quantities.
    if(vl.start()) {
        do {
            // Get the id and position of the site being considered.
            vl.pos(idx,x,y,z,r);
            idx -= start;
            std::vector<double> *tmp_vertices = with_vertices ? &(vertices_list[idx]) : &tmp_v;
            // Compute the voronoi cell.
            con.compute_cell(c,vl);
            // Compute the neighbours.
            c.neighbors(n_list[idx]);
            // Volume.
            vols.get()[idx] = c.volume();
            // Compute bounding box. Start by asking for the vertices.
            c.vertices(x,y,z,*tmp_vertices);
            // Init min/max bb.
            std::copy(tmp_vertices->begin(),tmp_vertices->begin() + 3,tmp_min);
            std::copy(tmp_vertices->begin(),tmp_vertices->begin() + 3,tmp_max);
            for (unsigned long i = 1u; i < tmp_vertices->size() / 3u; ++i) {
                for (unsigned j = 0; j < 3; ++j) {
                    if ((*tmp_vertices)[i * 3 + j] < tmp_min[j]) {
                        tmp_min[j] = (*tmp_vertices)[i * 3 + j];
                    }
                    if ((*tmp_vertices)[i * 3 + j] > tmp_max[j]) {
                        tmp_max[j] = (*tmp_vertices)[i * 3 + j];
                    }
                }
            }
            // Copy the bounding box into the output array.
            std::copy(tmp_min,tmp_min + 3,bb_m.get() + idx * 3);
            std::copy(tmp_max,tmp_max + 3,bb_M.get() + idx * 3);
        } while(vl.inc());
    }

    // Compute the max number of neighbours.
    *max_nn = std::max_element(n_list.begin(),n_list.end(),size_cmp<int>)->size();
    if (verbose) std::cout << "Max number of neighbours is: " << *max_nn << '\n';

    // Allocate space for flat array of neighbours.
    ptr_raii<int> neighs(static_cast<int *>(std::malloc(sizeof(int) * ncells * (*max_nn))));
    // Fill it in.
    for (idx = 0; idx < ncells; ++idx) {
        int *ptr = neighs.get() + (*max_nn) * idx;
        std::copy(n_list[idx].begin(),n_list[idx].end(),ptr);
        // Fill empty elements with -10.
        std::fill(ptr + n_list[idx].size(),ptr + (*max_nn),-10);
    }

    if (with_vertices) {
        // Compute the max number of vertices coordinates.
        *max_nv = std::max_element(vertices_list.begin(),vertices_list.end(),size_cmp<double>)->size();
        if (verbose) std::cout << "Max number of vertices coordinates is: " << *max_nv << '\n';

        // Allocate space for flat array of vertices.
        ptr_raii<double> verts(static_cast<double *>(std::malloc(sizeof(double) * ncells * (*max_nv))));
        // Fill it in.
        for (idx = 0; idx < ncells; ++idx) {
            double *ptr = verts.get() + (*max_nv) * idx;
            std::copy(vertices_list[idx].begin(),vertices_list[idx].end(),ptr);
            // Fill empty elements with nan.
            std::fill(ptr + vertices_list[idx].size(),ptr + (*max_nv),std::numeric_limits<double>::quiet_NaN());
        }

        // Assign the output quantity.
        *vertices = verts.release();
    } else {
        *max_nv = 0;
    }

    // Assign the output quantities.
    *volumes = vols.release();
    *bb_min = bb_m.release();
    *bb_max = bb_M.release();
    *neighbours = neighs.release();

    return NULL;

    } catch (const std::exception &e) {
        error_message = std::string("A C++ exception was raised while calling the voro++ wrapper. The full error message is: \"") + e.what() + "\".";
        return error_message.c_str();
    }
}
