#include <unistd.h>
#include <iostream>
#include <cstddef>
#include <iomanip>
#include <cassert>
#include <vector>
#include <cstdio>
#include <utility>
#include <math.h>

#include "allreduce.h"
#include "minimonitoring.h"

/* TODOs

- add clean version of the code:
    - without asserts
    - with simple loops, no optimization for contiguous lines, etc.

*/

MiniMon minimon;

using std::cout;
using std::setfill;
using std::setw;
using std::cerr;
using std::endl;
using std::setw;
using std::vector;

using TeamSpecT = dash::TeamSpec<3>;
using MatrixT = dash::NArray<double,3>;
using PatternT = typename MatrixT::pattern_type;
using StencilT = dash::halo::StencilPoint<3>;
using StencilSpecT = dash::halo::StencilSpec<StencilT,26>;
using CycleSpecT = dash::halo::GlobalBoundarySpec<3>;
using HaloT = dash::halo::HaloMatrixWrapper<MatrixT>;
using StencilOpT = dash::halo::StencilOperator<double,PatternT,StencilSpecT>;

/* for the smoothing operation, only the 6-point stencil is needed.
However, the prolongation operation also needs the */
StencilSpecT stencil_spec(
    StencilT(0.5, -1, 0, 0), StencilT(0.5, 1, 0, 0),
    StencilT(0.5,  0,-1, 0), StencilT(0.5, 0, 1, 0),
    StencilT(0.5,  0, 0,-1), StencilT(0.5, 0, 0, 1),

    StencilT(0.25, -1,-1, 0), StencilT( 0.25, 1, 1, 0),
    StencilT(0.25, -1, 0,-1), StencilT( 0.25, 1, 0, 1),
    StencilT(0.25,  0,-1,-1), StencilT( 0.25, 0, 1, 1),
    StencilT(0.25, -1, 1, 0), StencilT( 0.25, 1,-1, 0),
    StencilT(0.25, -1, 0, 1), StencilT( 0.25, 1, 0,-1),
    StencilT(0.25,  0,-1, 1), StencilT( 0.25, 0, 1,-1),

    StencilT(0.125, -1,-1,-1), StencilT( 0.125, 1,-1,-1),
    StencilT(0.125, -1,-1, 1), StencilT( 0.125, 1,-1, 1),
    StencilT(0.125, -1, 1,-1), StencilT( 0.125, 1, 1,-1),
    StencilT(0.125, -1, 1, 1), StencilT( 0.125, 1, 1, 1));

CycleSpecT cycle_spec(
    dash::halo::BoundaryProp::CUSTOM,
    dash::halo::BoundaryProp::CUSTOM,
    dash::halo::BoundaryProp::CUSTOM );

struct Level {

public:
  using SizeSpecT = dash::SizeSpec<3>;
  using DistSpecT = dash::DistributionSpec<3>;


    /* now with double-buffering. src_grid and src_halo should only be read,
    newgrid should only be written. dst_grid and dst_halo are only there to keep the other ones
    before both are swapped in swap() */

public:
    MatrixT* src_grid;
    MatrixT* dst_grid;
    MatrixT* rhs_grid; /* right hand side, doesn't need a halo */
    HaloT* src_halo;
    HaloT* dst_halo;
    StencilOpT* src_op;
    StencilOpT* dst_op;


    /* this are the values of the 7 non-zero matrix values -- only 4 different values, though,
    because the matrix is symmetric */
    double acenter, ax, ay, az;
    /* this is the factor for the right hand side, which is 0 at the finest grid. */
    double ff;
    /* Diagonal element of matrix M, which is the inverse of the diagonal of matrix A.
    This factor multiplies the defect in $ f - Au $. */
    double m;

    /* sz, sy, sx are the dimensions in meters of the grid excluding the boundary regions */
    double sz, sy, sx;

    /* the maximum time step according to the stability condition for the
    time simulation mode */
    double dt;

    /*
    lz, ly, lx are the dimensions in meters of the grid including the boundary regions,
    nz, ny, nx are th number of inner grid points per dimension, excluding the boundary regions,
    therefore, lz,ly,lx are discretized into (nz+2)*(ny+2)*(nx+2) grid points
    */
    Level( double lz, double ly, double lx,
           size_t nz, size_t ny, size_t nx,
           dash::Team& team, TeamSpecT teamspec ) :
            _grid_1( SizeSpecT( nz, ny, nx ), DistSpecT( dash::BLOCKED, dash::BLOCKED, dash::BLOCKED ), team, teamspec ),
            _grid_2( SizeSpecT( nz, ny, nx ), DistSpecT( dash::BLOCKED, dash::BLOCKED, dash::BLOCKED ), team, teamspec ),
            _rhs_grid( SizeSpecT( nz, ny, nx ), DistSpecT( dash::BLOCKED, dash::BLOCKED, dash::BLOCKED ), team, teamspec ),
        _halo_grid_1( _grid_1, cycle_spec, stencil_spec ),
        _halo_grid_2( _grid_2, cycle_spec, stencil_spec ),
        _stencil_op_1(_halo_grid_1.stencil_operator(stencil_spec)),
        _stencil_op_2(_halo_grid_2.stencil_operator(stencil_spec)),
        src_grid(&_grid_1), dst_grid(&_grid_2), rhs_grid(&_rhs_grid),
        src_halo(&_halo_grid_1), dst_halo(&_halo_grid_2),
        src_op(&_stencil_op_1),dst_op(&_stencil_op_2) {

        assert( 1 < nz );
        assert( 1 < ny );
        assert( 1 < nx );

        sz= lz;
        sy= ly;
        sx= lx;

        double hz= lz/(nz+1);
        double hy= ly/(ny+1);
        double hx= lx/(nx+1);

        /* This is the original setting for the linear system. */

        /* stability condition: r <= 1/2 with r= dt/h^2 ==> dt <= 1/2*h^2
        dtheta= ru*u_plus + ru*u_minus - 2*ru*u_center with ru=dt/hu^2 <= 1/2 */
        double hmin= std::min( hz, std::min( hy, hx ) );
        dt= 0.5*hmin*hmin;

        ax= -1.0/hx/hx;
        ay= -1.0/hy/hy;
        az= -1.0/hz/hz;
        acenter= -2.0*(ax+ay+az);
        m= 1.0 / acenter;

        ff= 1.0; /* factor for right-hand-side */

        for ( uint32_t a= 0; a < team.size(); a++ ) {
            if ( a == dash::myid() ) {
                if ( 0 == a ) {
                    cout << "Level " <<
                        "dim. " << lz << "m×" << ly << "m×" << lz << "m " <<
                        "in grid of " << nz << "×" << ny << "×" << nx <<
                        " h_= " << hz << "," << hy << "," << hx <<
                        " with team of " << team.size() <<
                        " ⇒ a_= " << acenter << "," << ax << "," << ay << "," << az <<
                        " , m= " << m << " , ff= " << ff <<endl;
                }
            }

            team.barrier();
        }
    }

    /***
    Alternative version of the constructor that takes the parent Level as the first argument.
    From this, it can get the original physical dimensions lz, ly, lx and the original
    grid distances hy, hy, hx.
    nz, ny, nx are th number of inner grid points per dimension, excluding the boundary regions
    */
    Level( const Level& parent,
           size_t nz, size_t ny, size_t nx,
           dash::Team& team, TeamSpecT teamspec ) :
            _grid_1( SizeSpecT( nz, ny, nx ), DistSpecT( dash::BLOCKED, dash::BLOCKED, dash::BLOCKED ), team, teamspec ),
            _grid_2( SizeSpecT( nz, ny, nx ), DistSpecT( dash::BLOCKED, dash::BLOCKED, dash::BLOCKED ), team, teamspec ),
            _rhs_grid( SizeSpecT( nz, ny, nx ), DistSpecT( dash::BLOCKED, dash::BLOCKED, dash::BLOCKED ), team, teamspec ),
        _halo_grid_1( _grid_1, cycle_spec, stencil_spec ),
        _halo_grid_2( _grid_2, cycle_spec, stencil_spec ),
        _stencil_op_1(_halo_grid_1.stencil_operator(stencil_spec)),
        _stencil_op_2(_halo_grid_2.stencil_operator(stencil_spec)),
        src_grid(&_grid_1), dst_grid(&_grid_2), rhs_grid(&_rhs_grid),
        src_halo(&_halo_grid_1), dst_halo(&_halo_grid_2),
        src_op(&_stencil_op_1),dst_op(&_stencil_op_2) {

        assert( 1 < nz );
        assert( 1 < ny );
        assert( 1 < nx );

        sz= parent.sz;
        sy= parent.sy;
        sx= parent.sx;

        ax= parent.ax;
        ay= parent.ay;
        az= parent.az;
        acenter= parent.acenter;
        ff= parent.ff;
        m= parent.m;
        dt= parent.dt;

        for ( uint32_t a= 0; a < team.size(); a++ ) {
            if ( a == dash::myid() ) {
                if ( 0 == a ) {
                    cout << "Level with a parent level " <<
                        "in grid of " << nz << "×" << ny << "×" << nx <<
                        " with team of " << team.size() <<
                        " ⇒ a_= " << acenter << "," << ax << "," << ay << "," << az <<
                        " , m= " << m << " , ff= " << ff << endl;
                }
            }

            team.barrier();
        }
    }

    Level() = delete;

    /** swap grid and halos for the double buffering scheme */
    void swap() {

        std::swap( src_halo, dst_halo );
        std::swap( src_grid, dst_grid );
        std::swap( src_op, dst_op );
    }

    double max_dt() const {

        /* stability condition: r <= 1/2 with r= dt/h^2 ==> dt <= 1/2*h^2
        dtheta= ru*u_plus + ru*u_minus - 2*ru*u_center with ru=dt/hu^2 <= 1/2 */
        cout << "    dt= " << dt << endl;
        return dt;
    }


private:
    MatrixT _grid_1;
    MatrixT _grid_2;
    HaloT _halo_grid_1;
    HaloT _halo_grid_2;
    MatrixT _rhs_grid;
    StencilOpT _stencil_op_1;
    StencilOpT _stencil_op_2;

};


void initgrid( Level& level ) {

    /* not strictly necessary but it also avoids NAN values */
    dash::fill( level.src_grid->begin(), level.src_grid->end(), 0.0 );
    dash::fill( level.dst_grid->begin(), level.dst_grid->end(), 0.0 );
    dash::fill( level.rhs_grid->begin(), level.rhs_grid->end(), 0.0 );

    level.src_grid->barrier();
}


/* apply boundary value settings, where the top and bottom planes have a
hot circle in the middle and everything else is cold */
void initboundary( Level& level ) {

    using index_t = dash::default_index_t;

    double gd= level.src_grid->extent(0);
    double gh= level.src_grid->extent(1);
    double gw= level.src_grid->extent(2);

    /* This way of setting boundaries uses subsampling on the top and bottom
    planes to determine the border values. This is another logical way that
    may be convenient sometimes. It guarantees that the boundary values on all
    the levels match.
    All other sides are constant at 0.0 degrees. The top an bottom circles are
    hot with 10.0 degrees. */

    auto lambda= [gd,gh,gw]( const std::array<index_t, 3>& coords ) {

        index_t z= coords[0];
        index_t y= coords[1];
        index_t x= coords[2];

        double ret= 1.0;

        /* for simplicity make every side uniform */

        if ( -1 == z || gd == z ) {

            /* radius differs on top and bottom plane */
            //double r= ( -1 == z ) ? 0.4 : 0.3;
            double r= 0.4;
            double r2= r*r;

            double lowvalue= 2.0;
            double highvalue= 9.0;

            double midx= 0.5;
            double midy= 0.5;

            /* At entry (x/gw,y/gh) we sample the
            rectangle [ x/gw,(x+1)/gw ) x [ y/gw, (y+1)/gh ) with m² points. */
            int32_t m= 3;
            int32_t m2= m*m;

            double sum= 0.0;
            double weight= 0.0;

            for ( double iy= -m+1; iy < m; iy++ ) {
                for ( double ix= -m+1; ix < m; ix++ ) {

                    double sx= (x+ix/m)/(gw-1);
                    double sy= (y+iy/m)/(gh-1);

                    double d2= (sx-midx)*(sx-midx) + (sy-midy)*(sy-midy);
                    sum += ( d2 <= r2 ) ? highvalue : lowvalue;
                    weight += 1.0;
                }
            }
            ret = sum / weight;
        }

        return ret;
    };

    level.src_halo->set_custom_halos( lambda );
    level.dst_halo->set_custom_halos( lambda );
}


/* sets all boundary values to 0, that is what is neede on the coarser grids */
void initboundary_zero( Level& level ) {

    using index_t = dash::default_index_t;

    auto lambda= []( const std::array<index_t, 3>& coords ) { return 0.0; };

    level.src_halo->set_custom_halos( lambda );
    level.dst_halo->set_custom_halos( lambda );
}


/* check some grid values for 3d mirror symmetry. This should hold for
appropriate boundary conditions and a correct solver.

Here we use global accesses for simplicity. */
bool check_symmetry( MatrixT& grid, double eps ) {

    if ( 0 == dash::myid() ) {

        size_t w= grid.extent(2);
        size_t h= grid.extent(1);
        size_t d= grid.extent(0);

        size_t m= std::min( std::min( w, h ), d ) /2;

        /* x-y-z diagonals */
        for ( size_t t= 0; t < m; ++t ) {

            double first= grid[d/2+t][h/2+t][w/2+t];

            if ( std::fabs( first - grid[d/2+t][h/2+t][w/2-t] ) > eps ) return false;
            if ( std::fabs( first - grid[d/2+t][h/2-t][w/2+t] ) > eps ) return false;
            if ( std::fabs( first - grid[d/2+t][h/2-t][w/2-t] ) > eps ) return false;
            if ( std::fabs( first - grid[d/2-t][h/2+t][w/2+t] ) > eps ) return false;
            if ( std::fabs( first - grid[d/2-t][h/2+t][w/2-t] ) > eps ) return false;
            if ( std::fabs( first - grid[d/2-t][h/2-t][w/2+t] ) > eps ) return false;
            if ( std::fabs( first - grid[d/2-t][h/2-t][w/2-t] ) > eps ) return false;
        }

        /* x-y diagonals */
        for ( size_t t= 0; t < m; ++t ) {

            double first= grid[d/2][h/2+t][w/2+t];

            if ( std::fabs( first - grid[d/2][h/2+t][w/2-t] ) > eps ) return false;
            if ( std::fabs( first - grid[d/2][h/2-t][w/2+t] ) > eps ) return false;
            if ( std::fabs( first - grid[d/2][h/2-t][w/2-t] ) > eps ) return false;
        }

        /* y-z diagonals */
        for ( size_t t= 0; t < m; ++t ) {

            double first= grid[d/2+t][h/2+t][w/2];

            if ( std::fabs( first - grid[d/2+t][h/2+t][w/2] ) > eps ) return false;
            if ( std::fabs( first - grid[d/2+t][h/2-t][w/2] ) > eps ) return false;
            if ( std::fabs( first - grid[d/2-t][h/2+t][w/2] ) > eps ) return false;
            if ( std::fabs( first - grid[d/2-t][h/2-t][w/2] ) > eps ) return false;
        }

        /* x-z diagonals */
        for ( size_t t= 0; t < m; ++t ) {

            double first= grid[d/2+t][h/2][w/2+t];

            if ( std::fabs( first - grid[d/2+t][h/2][w/2-t] ) > eps ) return false;
            if ( std::fabs( first - grid[d/2-t][h/2][w/2+t] ) > eps ) return false;
            if ( std::fabs( first - grid[d/2-t][h/2][w/2-t] ) > eps ) return false;
        }

    }

    return true;
}


void scaledownboundary( Level& fine, Level& coarse ) {

    using index_t = dash::default_index_t;

    assert( coarse.src_grid->extent(2)*2 == fine.src_grid->extent(2) );
    assert( coarse.src_grid->extent(1)*2 == fine.src_grid->extent(1) );
    assert( coarse.src_grid->extent(0)*2 == fine.src_grid->extent(0) );

    size_t dmax= coarse.src_grid->extent(0);
    size_t hmax= coarse.src_grid->extent(1);
    //size_t wmax= coarse.src_grid->extent(2);

    auto finehalo= fine.src_halo;

    auto lambda= [&finehalo,&dmax,&hmax]( const std::array<index_t, 3>& coord ) {

        auto coordf= coord;
        for( auto& c : coordf ) {
            if ( c > 0 ) c *= 2;
        }

        if ( -1 == coord[0] || dmax == coord[0] ) {

            /* z plane */
            return 0.25 * (
                *finehalo->halo_element_at_global( { coordf[0], coordf[1]+0, coordf[2]+0 } ) +
                *finehalo->halo_element_at_global( { coordf[0], coordf[1]+0, coordf[2]+1 } ) +
                *finehalo->halo_element_at_global( { coordf[0], coordf[1]+1, coordf[2]+0 } ) +
                *finehalo->halo_element_at_global( { coordf[0], coordf[1]+1, coordf[2]+1 } ) );

        } else if ( -1 == coord[1] || hmax == coord[1] ) {

            /* y plane */
            return 0.25 * (
                *finehalo->halo_element_at_global( { coordf[0]+0, coordf[1], coordf[2]+0 } ) +
                *finehalo->halo_element_at_global( { coordf[0]+0, coordf[1], coordf[2]+1 } ) +
                *finehalo->halo_element_at_global( { coordf[0]+1, coordf[1], coordf[2]+0 } ) +
                *finehalo->halo_element_at_global( { coordf[0]+1, coordf[1], coordf[2]+1 } ) );

        } else /* if ( -1 == coord[2] || wmax == coord[2] ) */ {

            /* x plane */
            return 0.25 * (
                *finehalo->halo_element_at_global( { coordf[0]+0, coordf[1]+0, coordf[2] } ) +
                *finehalo->halo_element_at_global( { coordf[0]+0, coordf[1]+1, coordf[2] } ) +
                *finehalo->halo_element_at_global( { coordf[0]+1, coordf[1]+0, coordf[2] } ) +
                *finehalo->halo_element_at_global( { coordf[0]+1, coordf[1]+1, coordf[2] } ) );

        }
    };

    coarse.src_halo->set_custom_halos( lambda );
    coarse.dst_halo->set_custom_halos( lambda );
}

void scaledown( Level& fine, Level& coarse ) {
    using signed_size_t = typename std::make_signed<size_t>::type;

    auto& finegrid= *fine.src_grid;
    auto& fine_rhs_grid= *fine.rhs_grid;
    auto& coarsegrid= *coarse.src_grid;
    auto& coarse_rhs_grid= *coarse.rhs_grid;
    auto& finehalo = *fine.src_halo;

    // stencil points for scale down with coefficients
    dash::halo::StencilSpec<StencilT,6> stencil_spec(
      StencilT(-fine.az, -1, 0, 0), StencilT(-fine.az, 1, 0, 0),
      StencilT(-fine.ay,  0,-1, 0), StencilT(-fine.ay, 0, 1, 0),
      StencilT(-fine.ax,  0, 0,-1), StencilT(-fine.ax, 0, 0, 1)
    );

    // scaledown
    minimon.start();

    assert( (coarsegrid.extent(2)+1) * 2 == finegrid.extent(2)+1 );
    assert( (coarsegrid.extent(1)+1) * 2 == finegrid.extent(1)+1 );
    assert( (coarsegrid.extent(0)+1) * 2 == finegrid.extent(0)+1 );

    const auto& extentc= coarsegrid.local.extents();
    const auto& cornerc= coarsegrid.pattern().global( {0,0,0} );
    const auto& extentf= finegrid.local.extents();
    const auto& cornerf= finegrid.pattern().global( {0,0,0} );

    assert( cornerc[0] * 2 == cornerf[0] );
    assert( cornerc[1] * 2 == cornerf[1] );
    assert( cornerc[2] * 2 == cornerf[2] );

    assert( 0 == cornerc[0] %2 );
    assert( 0 == cornerc[1] %2 );
    assert( 0 == cornerc[2] %2 );

    assert( extentc[0] * 2 == extentf[0] || extentc[0] * 2 +1 == extentf[0] );
    assert( extentc[1] * 2 == extentf[1] || extentc[1] * 2 +1 == extentf[1] );
    assert( extentc[2] * 2 == extentf[2] || extentc[2] * 2 +1 == extentf[2] );

    /* Here we  $ r= f - Au $ on the fine grid and 'straigth injection' to the
    rhs of the coarser grid in one. Therefore, we don't need a halo of the fine
    grid, because the stencil neighbor points on the fine grid are always there
    for a coarse grid point.
    According to the text book (Introduction to Algebraic Multigrid -- Course notes
    of an algebraic multigrid course at univertisty of Heidelberg in Wintersemester
    1998/99, Version 1.1 by Christian Wagner http://www.mgnet.org/mgnet/papers/Wagner/amgV11.pdf)
    there should by an extra factor 1/2^3 for the coarse value. But this doesn't seem to work,
    factor 4.0 works much better. */
    double extra_factor= 4.0;

    /* 1) start async halo exchange for fine grid*/
    finehalo.update_async();

    // iterates over all inner elements and calculates value for coarse rhs grid
    auto stencil_op_fine = fine.src_halo->stencil_operator(stencil_spec);
    for ( signed_size_t z= 1; z < extentc[0] - 1 ; z++ ) {
      for ( signed_size_t y= 1; y < extentc[1] - 1 ; y++ ) {
        for ( signed_size_t x= 1; x < extentc[2] - 1 ; x++ ) {
          coarse_rhs_grid.local[z][y][x] = extra_factor * (
              fine.ff * fine_rhs_grid.local[2*z+1][2*y+1][2*x+1] +
              stencil_op_fine.inner.get_value_at({2*z+1,2*y+1,2*x+1}, -fine.acenter));
        }
      }
    }

    /* 3) set coarse grid to 0.0 */
    dash::fill( coarsegrid.begin(), coarsegrid.end(), 0.0 );

    /* 4) wait for async halo exchange. Technically, we need only the back halos in every
    dimension and only for the front unit per dimension. However, we do the halo update
    collectvely to keep it managable. */

    finehalo.wait();

    auto& stencil_op_coarse = *coarse.src_op;
    auto* coarse_rhs_begin = coarse_rhs_grid.lbegin();
    // update all boundary elements for coarse rhs grid
    // coarse grid halo wrapper used to get coordinates for coarse rhs grid
    // elements
    auto bend = stencil_op_coarse.boundary.end();
    for( auto it = stencil_op_coarse.boundary.begin(); it != bend; ++it ) {
      const auto& coords = it.coords();
      // coarse coords to fine grid coords
      decltype(coords) coords_fine = {2*coords[0] + 1, 2*coords[1] + 1, 2*coords[2] + 1};
      // updates value for coarse rhs grid
      coarse_rhs_begin[it.lpos()] = extra_factor * (
        fine.ff * fine_rhs_grid.local[coords_fine[0]][coords_fine[1]][coords_fine[2]] +
        // default operation std::plus used for stencil point and center values
        stencil_op_fine.boundary.get_value_at(coords_fine, -fine.acenter));
    }

    minimon.stop( "scaledown", finegrid.team().size(), finegrid.local_size() );
}

/* this version uses a correct prolongation from the coarser grid of (2^n)^3 to (2^(n+1))^3
elements. Note that it is 2^n elements per dimension instead of 2^n -1!
This version loops over the coarse grid */
//void scaleup_loop_coarse( Level& coarse, Level& fine ) {
void scaleup( Level& coarse, Level& fine ) {
    using signed_size_t = typename std::make_signed<size_t>::type;

    MatrixT& coarsegrid= *coarse.src_grid;
    MatrixT& finegrid= *fine.src_grid;

    // scaleup
    minimon.start();

    assert( (coarsegrid.extent(2)+1) * 2 == finegrid.extent(2)+1 );
    assert( (coarsegrid.extent(1)+1) * 2 == finegrid.extent(1)+1 );
    assert( (coarsegrid.extent(0)+1) * 2 == finegrid.extent(0)+1 );

    const auto& extentc= coarsegrid.pattern().local_extents();
    const auto& cornerc= coarsegrid.pattern().global( {0,0,0} );
    const auto& extentf= finegrid.pattern().local_extents();
    const auto& cornerf= finegrid.pattern().global( {0,0,0} );

    assert( cornerc[0] * 2 == cornerf[0] );
    assert( cornerc[1] * 2 == cornerf[1] );
    assert( cornerc[2] * 2 == cornerf[2] );

    assert( 0 == cornerc[0] %2 );
    assert( 0 == cornerc[1] %2 );
    assert( 0 == cornerc[2] %2 );

    assert( extentc[0] * 2 == extentf[0] || extentc[0] * 2 +1 == extentf[0] );
    assert( extentc[1] * 2 == extentf[1] || extentc[1] * 2 +1 == extentf[1] );
    assert( extentc[2] * 2 == extentf[2] || extentc[2] * 2 +1 == extentf[2] );

    /* if last element in coarse grid per dimension has no 2*i+2 element in
    the local fine grid, then handle it as a separate loop using halo.
    sub[i] is always 0 or 1 */
    std::array< size_t, 3 > sub;
    for ( uint32_t i= 0; i < 3; ++i ) {
         sub[i]= ( extentc[i] * 2 == extentf[i] ) ? 1 : 0;
    }

    /* start async halo exchange for coarse grid*/
    coarse.src_halo->update_async();

    /* second loop over the coarse grid and add the contributions to the
    fine grid elements */

    /* this is the iterator-ized version of the code */

    auto& stencil_op_fine = *fine.src_op;
    // set inner elements
    for ( signed_size_t z= 1; z < extentc[0] - 1; z++ ) {
      for ( signed_size_t y= 1; y < extentc[1] - 1; y++ ) {
        for ( signed_size_t x= 1; x < extentc[2] - 1; x++ ) {
          stencil_op_fine.inner.set_values_at({2*z+1, 2*y+1,2*x+1},
          coarsegrid.local[z][y][x], 1.0,std::plus<double>());
        }
      }
    }

    // set values for boundary elements, halo elements are excluded
    auto bend = coarse.src_op->boundary.end();
    for (auto it = coarse.src_op->boundary.begin(); it != bend; ++it ) {
      const auto& coords = it.coords();
      stencil_op_fine.boundary.set_values_at( {2*coords[0]+1, 2*coords[1]+1,
          2*coords[2]+1}, *it, 1.0, std::plus<double>());
    }

    /* wait for async halo exchange */
    coarse.src_halo->wait();

    /* do the remaining updates with contributions from the coarse halo
	for 6 planes, 12 edges, and 8 corners */

    const auto& halo_block = coarse.src_halo->halo_block();
    const auto& view = halo_block.view();
    const auto& specs = stencil_op_fine.stencil_spec().specs();

    // iterates over all halo regions to find and get all halo regions before
    // center -> only needed for element update
    for(const auto& region : halo_block.halo_regions()) {

      // region filter -> custom halo regions and regions behind center are
      // excluded
      if(region.is_custom_region() ||
         (region.spec()[0] == 2 && sub[0]) ||
         (region.spec()[1] == 2 && sub[1]) ||
         (region.spec()[2] == 2 && sub[2])) {
        continue;
      }

      // iterates over all region elements und updates all  elements in fine
      // grid, except halo elements
      auto region_end = region.end();
      for(auto it = region.begin(); it != region_end; ++it) {
        auto coords = it.gcoords();
        // pointer to halo element
        double* halo_element = coarse.src_halo->halo_element_at_global(coords);

        // if halo element == nullptr no halo element exists for the given
        // coordinates -> continue with next element
        if(halo_element == nullptr)
          continue;

        // convert global coordinate to local and fine grid coordinate
        for(auto d = 0; d < 3; d++) {
          coords[d] -= view.offset(d); // to local
          if(coords[d] < 0 )
            continue;

          coords[d] = coords[d] * 2 + 1; // to fine grid
        }

        // iterates over all stencil points
        for(auto i = 0; i < specs.size(); ++i) {
          // returns pair -> first stencil_point adjusted coords, second check for halo
          auto coords_stencilp = specs[i].stencil_coords_check_abort(coords,
              stencil_op_fine.view_local());
          /*
           * Checks if stencil point points to a local memory element.
           * if its points to a halo element continue with next stencil point
           */
          if(coords_stencilp.second)
            continue;

          // set new value for stencil point element
          auto offset =  stencil_op_fine.get_offset(coords_stencilp.first);
          finegrid.lbegin()[offset] += specs[i].coefficient() * *halo_element;
        }
      }
    }

    /* how to calculate the number of flops here: for every element there are 2 flop (one add, one mul), 
    then calculate the number of finegrid points that receive a contribution from a coarse grid point with 
    coefficient 1.0, 0.5, 0.25, an 0.125 separately. Consider the case where a unit is last in the distributions
    in any dimension, which is marked with 'sub[.]==1'. In those cases change '(extentc[.]-1)' --> '(extentc[.]-1+sub[.])'
    Then sum them up and simplify. */
    minimon.stop( "scaleup", coarsegrid.team().size() /* param */, coarsegrid.local_size() /* elem */, 
        (2*extentc[0]-1+sub[0])*(2*extentc[1]-1+sub[1])*(2*extentc[2]-1+sub[2])*2 /* flops */ );
}

void transfertofewer( Level& source /* with larger team*/, Level& dest /* with smaller team */ ) {

    /* should only be called by the smaller team */
    assert( 0 == dest.src_grid->team().position() );

    cout << "unit " << dash::myid() << " transfertofewer" << endl;

    /* we need to find the coordinates that the local unit needs to receive
    from several other units that are not in this team */

    /* we can safely assume that the source blocks are copied entirely */

    std::array< long int, 3 > corner= dest.src_grid->pattern().global( {0,0,0} );
    std::array< long unsigned int, 3 > sizes= dest.src_grid->pattern().local_extents();

    /*
    cout << "    start coord: " <<
        corner[0] << ", "  << corner[1] << ", " << corner[2] << endl;
    cout << "    extents: " <<
            sizes[0] << ", "  << sizes[1] << ", " << sizes[2] << endl;
    cout << "    dest local  dist " << dest.src_grid->lend() - dest.src_grid->lbegin() << endl;
    cout << "    dest global dist " << dest.src_grid->end() - dest.src_grid->begin() << endl;
    cout << "    src  local  dist " << source.src_grid->lend() - source.src_grid->lbegin() << endl;
    cout << "    src  global dist " << source.src_grid->end() - source.src_grid->begin() << endl;
    */

    /* Can I do this any cleverer than loops over the n-1 non-contiguous
    dimensions and then a dash::copy for the 1 contiguous dimension? */

    /*
    for ( uint32_t z= 0; z < sizes[0]; z++ ) {
        for ( uint32_t y= 0; y < sizes[1]; y++ ) {
            for ( uint32_t x= 0; x < sizes[2]; x++ ) {

                (*dest.src_grid)(z,y,x)= (*source.src_grid)(z,y,x);
            }
        }
    }
    */

    for ( uint32_t z= 0; z < sizes[0]; z++ ) {
        for ( uint32_t y= 0; y < sizes[1]; y++ ) {

            size_t offset= ((corner[0]+z)*sizes[1]+y)*sizes[2];
            std::copy( source.src_grid->begin() + offset, source.src_grid->begin() + offset + sizes[2],
                &dest.src_grid->local[z][y][0] );
            std::copy( source.rhs_grid->begin() + offset, source.rhs_grid->begin() + offset + sizes[2],
                &dest.rhs_grid->local[z][y][0] );

            //dash::copy( start, start + sizes[2], &dest.src_grid->local[z][y][0] );
            //dash::copy( source.grid.begin()+40, source.grid.begin()+48, buf );
        }
    }

}


void transfertomore( Level& source /* with smaller team*/, Level& dest /* with larger team */ ) {

    /* should only be called by the smaller team */
    assert( 0 == source.src_grid->team().position() );

cout << "unit " << dash::myid() << " transfertomore" << endl;

    /* we need to find the coordinates that the local unit needs to receive
    from several other units that are not in this team */

    /* we can safely assume that the source blocks are copied entirely */

    std::array< long int, 3 > corner= source.src_grid->pattern().global( {0,0,0} );
    std::array< long unsigned int, 3 > sizes= source.src_grid->pattern().local_extents();

    /*
    cout << "    start coord: " <<
        corner[0] << ", "  << corner[1] << ", " << corner[2] << endl;
    cout << "    extents: " <<
            sizes[0] << ", "  << sizes[1] << ", " << sizes[2] << endl;
    cout << "    dest local  dist " << source.src_grid->lend() - source.src_grid->lbegin() << endl;
    cout << "    dest global dist " << source.src_grid->end() - source.src_grid->begin() << endl;
    cout << "    src  local  dist " << dest.src_grid->lend() - dest.src_grid->lbegin() << endl;
    cout << "    src  global dist " << dest.src_grid->end() - dest.src_grid->begin() << endl;
    */

    /* stupid but functional version for the case with only one unit in the smaller team, very slow individual accesses */
    /*
    for ( uint32_t z= 0; z < sizes[0]; z++ ) {
        for ( uint32_t y= 0; y < sizes[1]; y++ ) {
            for ( uint32_t x= 0; x < sizes[2]; x++ ) {

                (*dest.src_grid)(z,y,x)= (*source.src_grid)(z,y,x);
            }
        }
    }
    */
    for ( uint32_t z= 0; z < sizes[0]; z++ ) {
        for ( uint32_t y= 0; y < sizes[1]; y++ ) {

            size_t offset= ((corner[0]+z)*sizes[1]+y)*sizes[2];
            std::copy( &source.src_grid->local[z][y][0], &source.src_grid->local[z][y][0] + sizes[2],
                dest.src_grid->begin() + offset );
            std::copy( &source.rhs_grid->local[z][y][0], &source.rhs_grid->local[z][y][0] + sizes[2],
                dest.rhs_grid->begin() + offset );

            //dash::copy( start, start + sizes[2], &dest.src_grid->local[z][y][0] );
            //dash::copy( source.grid.begin()+40, source.grid.begin()+48, buf );
        }
    }

    //std::copy( source.src_grid->begin(), source.src_grid->end(), dest.src_grid->begin() );
}

static inline double update_inner_dash( Level& level, double coeff )
{
    double ax= level.ax;
    double ay= level.ay;
    double az= level.az;
    double ac= level.acenter;
    double ff= level.ff;
    double m= level.m;

    const double c= coeff;

    double localres= 0.0;
    auto p_rhs=   level.rhs_grid->lbegin();
    level.src_op->inner.update(level.dst_grid->lbegin(),
        [&](auto* center, auto* center_dst, auto offset, const auto& offsets) {
                double dtheta= m * (
                    ff * p_rhs[offset] -
                    ax * ( center[offsets[4]] + center[offsets[5]] ) -
                    ay * ( center[offsets[2]] + center[offsets[3]] ) -
                    az * ( center[offsets[0]] + center[offsets[1]] ) -
                    ac * *center );
                localres= std::max( localres, std::fabs( dtheta ) );
                *center_dst = *center + c * dtheta;
        });

    return localres;
}


template<
    unsigned int NTHREADS
>
struct UpdateInnerAcc
{
    void operator()( Level& level, double coeff, size_t z, double* res ) const
    {
        size_t lh= level.src_grid->local.extent(1);
        size_t lw= level.src_grid->local.extent(2);

        double ax= level.ax;
        double ay= level.ay;
        double az= level.az;
        double ac= level.acenter;
        double ff= level.ff;
        double m= level.m;

        const double c= coeff;

        auto layer_size = lw * lh;

        const double* __restrict p_src=  level.src_grid->lbegin();
        const double* __restrict p_rhs=   level.rhs_grid->lbegin();
        double* __restrict p_dst= level.dst_grid->lbegin();

        std::array<double, NTHREADS> localres;
        #pragma unroll
        for (unsigned int tidx = 0; tidx < NTHREADS; tidx++)
            localres[tidx]= 0.0;

        for ( size_t y= 0; y < lh-2; y++ ) {
            auto core_offset = (z + 1) * layer_size + lw + 1
                               + y * lw;

            /* this should eventually be done with Alpaka or Kokkos to look
            much nicer but still be fast */

            #pragma unroll
            for (unsigned int tidx = 0; tidx < NTHREADS; tidx++) {
                for ( size_t x= tidx; x < lw-2; x+=NTHREADS ) {

                    /*
                    stability condition: r <= 1/2 with r= dt/h^2 ==> dt <= 1/2*h^2
                    dtheta= ru*u_plus + ru*u_minus - 2*ru*u_center with ru=dt/hu^2 <= 1/2
                    */
                    double dtheta= m * (
                        ff * p_rhs[core_offset+x] -
                        ax * ( p_src[core_offset+x-1] +          p_src[core_offset+x+1] ) -
                        ay * ( p_src[core_offset+x-lw] +         p_src[core_offset+x+lw] ) -
                        az * ( p_src[core_offset+x-layer_size] + p_src[core_offset+x+layer_size] ) -
                        ac * p_src[core_offset+x] );
                    p_dst[core_offset+x]= p_src[core_offset+x] + c * dtheta;

                    localres[tidx] = std::max( localres[tidx], std::fabs( dtheta ) );
                }
            }
        }

        // start reduction
        auto n = NTHREADS / 2;
        while (n) {
            #pragma unroll
            for (unsigned int tidx = 0; tidx < n && (tidx + n) < NTHREADS; tidx++)
                localres[tidx] = std::max( localres[tidx], localres[tidx + n] );
            n /= 2;
        }

        *res = localres[0];
    }
};

template<
    unsigned int NTHREADS
>
static inline double update_inner_acc( Level& level, double coeff )
{
    size_t ld= level.src_grid->local.extent(0);

    UpdateInnerAcc<NTHREADS> kernel;
    double localres= 0.0;
    for ( size_t z= 0; z < ld-2; z++ ) {
        double localresz= 0.0;
        kernel(level, coeff, z, &localresz);
        localres = std::max( localres, localresz );
    }

    return localres;
}

/**
Smoothen the given level from oldgrid+src_halo to newgrid. Call Level::swap() at the end.

The parallel global residual is returned as a return parameter, but only
if it is not NULL because then the expensive parallel reduction is just avoided.
*/
double smoothen( Level& level, Allreduce& res, double coeff= 1.0 ) {
    SCOREP_USER_FUNC()

    uint32_t par= level.src_grid->team().size();

    // smoothen
    minimon.start();

    level.src_grid->barrier();

    size_t ld= level.src_grid->local.extent(0);
    size_t lh= level.src_grid->local.extent(1);
    size_t lw= level.src_grid->local.extent(2);

    double ax= level.ax;
    double ay= level.ay;
    double az= level.az;
    double ac= level.acenter;
    double ff= level.ff;
    double m= level.m;

    const double c= coeff;

    // async halo update
    level.src_halo->update_async();

    // smoothen_inner
    minimon.start();

    // update inner

    /* the start value for both, the y loop and the x loop is 1 because either there is
    a border area next to the halo -- then the first column or row is covered below in
    the border update -- or there is an outside border -- then the first column or row
    contains the boundary values. */
#if 1
    double localres = update_inner_dash(level, coeff);
#else
    double localres = update_inner_acc<1>(level, coeff);
#endif
    minimon.stop( "smoothen_inner", par, /* elements */ (ld-2)*(lh-2)*(lw-2), /* flops */ 16*(ld-2)*(lh-2)*(lw-2), /*loads*/ 7*(ld-2)*(lh-2)*(lw-2), /* stores */ (ld-2)*(lh-2)*(lw-2) );

    // smoothen_wait
    minimon.start();
    // wait for async halo update

    level.src_halo->wait();

    minimon.stop( "smoothen_wait", par, /* elements */ ld*lh*lw );

    // smoothen_collect
    minimon.start();

    /* unit 0 (of any active team) waits until all local residuals from all
    other active units are in */
    res.collect_and_spread( level.src_grid->team() );

    minimon.stop( "smoothen_collect", par );

    // smoothen_outer
    minimon.start();

    /// begin pointer of local block, needed because halo border iterator is read-only
    auto grid_local_begin= level.dst_grid->lbegin();
    auto rhs_grid_local_begin= level.rhs_grid->lbegin();

    auto bend = level.src_op->boundary.end();
    // update border area
    for( auto it = level.src_op->boundary.begin(); it != bend; ++it ) {

        double dtheta= m * (
            ff * rhs_grid_local_begin[ it.lpos() ] -
            ax * ( it.value_at(4) + it.value_at(5) ) -
            ay * ( it.value_at(2) + it.value_at(3) ) -
            az * ( it.value_at(0) + it.value_at(1) ) -
            ac * *it );
        grid_local_begin[ it.lpos() ]= *it + c * dtheta;

        localres= std::max( localres, std::fabs( dtheta ) );
    }

    minimon.stop( "smoothen_outer", par, /* elements */ 2*(ld*lh+lh*lw+lw*ld),
        /* flops */ 16*(ld*lh+lh*lw+lw*ld), /*loads*/ 7*(ld*lh+lh*lw+lw*ld), /* stores */ (ld*lh+lh*lw+lw*ld) );

    // smoothen_wait_res
    minimon.start();

    res.wait( level.src_grid->team() );

    /* global residual from former iteration */
    double oldres= res.get();

    res.set( &localres, level.src_grid->team() );

    minimon.stop( "smoothen_wait_res", par );

    level.swap();

    minimon.stop( "smoothen", par, /* elements */ ld*lh*lw,
        /* flops */ 16*ld*lh*lw, /*loads*/ 7*ld*lh*lw, /* stores */ ld*lh*lw );

    return oldres;
}

//#define DETAILOUTPUT 1

template<typename Iterator>
void recursive_cycle( Iterator it, Iterator itend,
        uint32_t beta, uint32_t gamma, double epsilon, Allreduce& res ) {
    SCOREP_USER_FUNC()

    Iterator itnext( it );
    ++itnext;
    /* reached end of recursion? */
    if ( itend == itnext ) {

        /* smoothen completely  */
        uint32_t j= 0;
        res.reset( (*it)->src_grid->team() );
        while ( res.get() > epsilon ) {
            /* need global residual for iteration count */
            smoothen( **it, res );

            j++;
        }
        if ( 0 == dash::myid() ) {
            cout << "smoothing " <<
            (*it)->src_grid->extent(2) << "×" <<
            (*it)->src_grid->extent(1) << "×" <<
            (*it)->src_grid->extent(0) << " coarsest " << j << " times with residual " << res.get() << endl;
        }

        return;
    }

    /* stepped on the dummy level? ... which is there to signal that it is not
    the end of the parallel recursion on the coarsest level but a subteam is
    going on to solve the coarser levels and this unit is not in that subteam.
    ... sounds complicated, is complicated, change only if you know what you
    are doing. */
    if ( NULL == *itnext ) {

        /* barrier 'Alice', belongs together with the next barrier 'Bob' below */
        (*it)->src_grid->team().barrier();

        cout << "all meet again here: I'm passive unit " << dash::myid() << endl;

        return;
    }

    /* stepped on a transfer level? */
    if ( (*it)->src_grid->team().size() != (*itnext)->src_grid->team().size() ) {

        /* only the members of the reduced team need to work, all others do siesta. */
        //if ( 0 == (*itnext)->grid.team().position() )
        assert( 0 == (*itnext)->src_grid->team().position() );
        {

            cout << "transfer to " <<
                (*it)->src_grid->extent(2) << "×" <<
                (*it)->src_grid->extent(1) << "×" <<
                (*it)->src_grid->extent(0) << " with " << (*it)->src_grid->team().size() << " units "
                " ⇒ " <<
                (*itnext)->src_grid->extent(2) << "×" <<
                (*itnext)->src_grid->extent(1) << "×" <<
                (*itnext)->src_grid->extent(0) << " with " << (*itnext)->src_grid->team().size() << " units " << endl;

            transfertofewer( **it, **itnext );

            /* don't apply a gamma != 1 here! */
            recursive_cycle( itnext, itend, beta, gamma, epsilon, res );

            cout << "transfer back " <<
            (*itnext)->src_grid->extent(2) << "×" <<
            (*itnext)->src_grid->extent(1) << "×" <<
            (*itnext)->src_grid->extent(0) << " with " << (*itnext)->src_grid->team().size() << " units "
            " ⇒ " <<
            (*it)->src_grid->extent(2) << "×" <<
            (*it)->src_grid->extent(1) << "×" <<
            (*it)->src_grid->extent(0) << " with " << (*it)->src_grid->team().size() << " units " <<  endl;

            transfertomore( **itnext, **it );
        }

        /* barrier 'Bob', belongs together with the previous barrier 'Alice' above */
        (*it)->src_grid->team().barrier();


        cout << "all meet again here: I'm active unit " << dash::myid() << endl;
        return;
    }


    /* **** normal recursion **** **** **** **** **** **** **** **** **** */

    /* smoothen fixed number of times */
    uint32_t j= 0;
    res.reset( (*it)->src_grid->team() );
    while ( res.get() > epsilon && j < beta ) {

        /* need global residual for iteration count */
        smoothen( **it, res );

        j++;
    }
    if ( 0 == dash::myid()  ) {
        cout << "smoothing " <<
            (*it)->src_grid->extent(2) << "×" <<
            (*it)->src_grid->extent(1) << "×" <<
            (*it)->src_grid->extent(0) << " on way down " << j << " times with residual " << res.get() << endl;
    }

    /* scale down */
    if ( 0 == dash::myid() ) {
        cout << "scale down " <<
            (*it)->src_grid->extent(2) << "×" <<
            (*it)->src_grid->extent(1) << "×" <<
            (*it)->src_grid->extent(0) <<
            " ⇒ " <<
            (*itnext)->src_grid->extent(2) << "×" <<
            (*itnext)->src_grid->extent(1) << "×" <<
            (*itnext)->src_grid->extent(0) << endl;
    }

    scaledown( **it, **itnext );

    /* recurse  */
    for ( uint32_t g= 0; g < gamma; ++g ) {
        recursive_cycle( itnext, itend, beta, gamma, epsilon, res );
    }

    /* scale up */
    if ( 0 == dash::myid() ) {
        cout << "scale up " <<
            (*itnext)->src_grid->extent(2) << "×" <<
            (*itnext)->src_grid->extent(1) << "×" <<
            (*itnext)->src_grid->extent(0) <<
            " ⇒ " <<
            (*it)->src_grid->extent(2) << "×" <<
            (*it)->src_grid->extent(1) << "×" <<
            (*it)->src_grid->extent(0) << endl;
    }
    scaleup( **itnext, **it );

    j= 0;
    res.reset( (*it)->src_grid->team() );
    while ( res.get() > epsilon && j < beta ) {

        /* need global residual for iteration count */
        smoothen( **it, res );

        j++;
    }
    if ( 0 == dash::myid() ) {
        cout << "smoothing " <<
            (*it)->src_grid->extent(2) << "×" <<
            (*it)->src_grid->extent(1) << "×" <<
            (*it)->src_grid->extent(0) << " on way up " << j << " times with residual " << res.get() << endl;
    }
}


void smoothen_final( Level& level, double epsilon, Allreduce& res ) {
    SCOREP_USER_FUNC()

    uint64_t par= level.src_grid->team().size() ;

    // smooth_final
    minimon.start();

    uint32_t j= 0;
    res.reset( level.src_grid->team() );
    while ( res.get() > epsilon ) {

        smoothen( level, res );
        j++;
    }
    if ( 0 == dash::myid() ) {
        cout << "smoothing: " << j << " steps finest with residual " << res.get() << endl;
    }

/////////////////////////////////////////
#ifdef DETAILOUTPUT
if ( 0 == dash::myid()  ) {
    cout << "== after final smoothing ==" << endl;
    cout << "  src_grid" << endl;
    level.printout();
    cout << "  rhs_grid" << endl;
    level.printout_rhs();
}
#endif /* DETAILOUTPUT */

    minimon.stop( "smooth_final", par );
}


double do_multigrid_iteration( uint32_t howmanylevels, double eps, std::array< double, 3 >& dim ) {
    SCOREP_USER_FUNC()

    // setup
    minimon.start();

    TeamSpecT teamspec( dash::Team::All().size(), 1, 1 );
    teamspec.balance_extents();

    /* determine factors for width and height such that every unit has a power of two
    extent in every dimension and that the area is close to a square
    with aspect ratio \in [0,75,1,5] */

    vector<Level*> levels;
    levels.reserve( howmanylevels );

    if ( 0 == dash::myid() ) {

        cout << "run multigrid iteration with " << dash::Team::All().size() << " units "
            "for with grids from " <<
            2 << "×" <<
            2 << "×" <<
            2 <<
            " to " <<
            ((1<<(howmanylevels))-1) << "×" <<
            ((1<<(howmanylevels))-1) << "×" <<
            ((1<<(howmanylevels))-1) <<
            endl;
    }

    /* finest grid needs to be larger than 2*teamspec per dimension,
    that means local grid is >= 2 elements */
    assert( (1<<(howmanylevels))-1 >= 2*teamspec.num_units(0) );
    assert( (1<<(howmanylevels))-1 >= 2*teamspec.num_units(1) );
    assert( (1<<(howmanylevels))-1 >= 2*teamspec.num_units(2) );

    /* create all grid levels, starting with the finest and ending with 2x2,
    The finest level is outside the loop because it is always done by dash::Team::All() */

    if ( 0 == dash::myid() ) {
        cout << "finest level is " <<
            (1<<(howmanylevels))-1 << "×" <<
            (1<<(howmanylevels))-1 << "×" <<
            (1<<(howmanylevels))-1 <<
            " distributed over " <<
            teamspec.num_units(0) << "×" <<
            teamspec.num_units(1) << "×" <<
            teamspec.num_units(2) << " units" << endl;
    }

    levels.push_back( new Level( dim[0], dim[1], dim[2],
        (1<<(howmanylevels))-1,
        (1<<(howmanylevels))-1,
        (1<<(howmanylevels))-1,
        dash::Team::All(), teamspec ) );

    /* only do initgrid on the finest level, use scaledownboundary for all others */
    initboundary( *levels.back() );

    dash::barrier();

    --howmanylevels;
    while ( (1<<(howmanylevels))-1 >= 2*teamspec.num_units(0) &&
            (1<<(howmanylevels))-1 >= 2*teamspec.num_units(1) &&
            (1<<(howmanylevels))-1 >= 2*teamspec.num_units(2) ) {

        /*
        if ( 0 == dash::myid() ) {
            cout << "compute level " << l << " is " <<
                (1<<(howmanylevels))-1 << "×" <<
                (1<<(howmanylevels))-1 << "×" <<
                (1<<(howmanylevels))-1 <<
                " distributed over " <<
                teamspec.num_units(0) << "×" <<
                teamspec.num_units(1) << "×" <<
                teamspec.num_units(2) << " units" << endl;
        }
        */

        /* do not try to allocate >= 8GB per core -- try to prevent myself
        from running too big a simulation on my laptop */
        assert( ((1<<(howmanylevels))-1) *
            ((1<<(howmanylevels))-1) *
            ((1<<(howmanylevels))-1) < dash::Team::All().size() * (1<<27) );

        Level& previouslevel= *levels.back();

        levels.push_back(
            new Level( previouslevel,
                       (1<<(howmanylevels))-1,
                       (1<<(howmanylevels))-1,
                       (1<<(howmanylevels))-1,
                       dash::Team::All(), teamspec ) );

        /* scaledown boundary instead of initializing it from the same
        procedure, because this is very prone to subtle mistakes which
        makes the entire multigrid algorithm misbehave. */
        //scaledownboundary( previouslevel, *levels.back() );

        initboundary_zero( *levels.back() );

        dash::barrier();
        --howmanylevels;
    }

    /* here all units and all teams meet again, those that were active for the coarsest
    levels and those that were dormant */
    dash::Team::All().barrier();

    /* Fill finest level. Strictly, we don't need to set any initial values here
    but we do it for demonstration in the graphical output */
    initgrid( *levels.front() );

    dash::Team::All().barrier();

    Allreduce res( dash::Team::All() );

    minimon.stop( "setup", dash::Team::All().size() );

    //v_cycle( levels.begin(), levels.end(), 20, eps, res );
    //recursive_cycle( levels.begin(), levels.end(), 20, 1 /* 1 for v cycle */, eps, res );

    // algorithm
    minimon.start();

    if ( 0 == dash::myid()  ) {
        cout << "start w-cycle with res " << eps << endl << endl;
    }
    //w_cycle( levels.begin(), levels.end(), 20, eps, res );
    recursive_cycle( levels.begin(), levels.end(), 20, 2 /* 2 for w cycle */, eps, res );
    dash::Team::All().barrier();


    if ( 0 == dash::myid()  ) {
        cout << "final smoothing with res " << eps << endl;
    }
    smoothen_final( *levels.front(), eps, res );

    minimon.stop( "algorithm", dash::Team::All().size() );

    dash::Team::All().barrier();

    if ( 0 == dash::myid() ) {

        if ( ! check_symmetry( *levels.front()->src_grid, eps ) ) {

            cout << "test for asymmetry of soution failed!" << endl;
        }
    }

    return res.get();
}


/* elastic mode runs but still seems to have errors in it */
double do_multigrid_elastic( uint32_t howmanylevels, double eps, std::array< double, 3 >& dim, int split ) {

    // setup
    minimon.start();

    TeamSpecT teamspec( dash::Team::All().size(), 1, 1 );
    teamspec.balance_extents();

    /* determine factors for width and height such that every unit has a power of two
    extent in every dimension and that the area is close to a square
    with aspect ratio \in [0,75,1,5] */

    uint32_t factor_z= 1;
    uint32_t factor_y= 1;
    uint32_t factor_x= 1;

    vector<Level*> levels;
    levels.reserve( howmanylevels );

    if ( 0 == dash::myid() ) {

        cout << "run elastic multigrid iteration with " << dash::Team::All().size() << " units "
            "for with grids from " <<
            2*factor_z << "×" <<
            2*factor_y << "×" <<
            2* factor_x <<
            " to " <<
            ((1<<(howmanylevels))-1)*factor_z << "×" <<
            ((1<<(howmanylevels))-1)*factor_y << "×" <<
            ((1<<(howmanylevels))-1)*factor_x <<
            " splitting every " << split << (split == 1 ? "st" : split == 2 ? "nd" : split == 3 ? "rd" : "th") << " level" <<
            endl << endl;
    }

    /* create all grid levels, starting with the finest and ending with 2x2,
    The finest level is outside the loop because it is always done by dash::Team::All() */

    if ( 0 == dash::myid() ) {
        cout << "finest level is " <<
            ((1<<(howmanylevels))-1)*factor_z << "×" <<
            ((1<<(howmanylevels))-1)*factor_y << "×" <<
            ((1<<(howmanylevels))-1)*factor_x <<
            " distributed over " <<
            teamspec.num_units(0) << "×" <<
            teamspec.num_units(1) << "×" <<
            teamspec.num_units(2) << " units" << endl;
    }

    levels.push_back( new Level( dim[0], dim[1], dim[2],
        ((1<<(howmanylevels))-1)*factor_z ,
        ((1<<(howmanylevels))-1)*factor_y ,
        ((1<<(howmanylevels))-1)*factor_x ,
        dash::Team::All(), teamspec ) );

    /* only do initgrid on the finest level, use scaledownboundary for all others */
    initboundary( *levels.back() );

    dash::barrier();

    --howmanylevels;
    int split_steps=1;
    while ( 0 < howmanylevels ) {

        dash::Team& previousteam= levels.back()->src_grid->team();
        dash::Team& currentteam= ( split_steps++ % split == 0 && previousteam.size() > 1 ) ? previousteam.split(8) : previousteam;
        TeamSpecT localteamspec( currentteam.size(), 1, 1 );
        localteamspec.balance_extents();

        /* this is the real iteration condition for this loop! */
        if ( (1<<(howmanylevels))-1 < 2*localteamspec.num_units(0) ||
                (1<<(howmanylevels))-1 < 2*localteamspec.num_units(1) ||
                (1<<(howmanylevels))-1 < 2*localteamspec.num_units(2) ) break;

        if ( 0 == currentteam.position() ) {

            if ( previousteam.size() != currentteam.size() ) {

                /* the team working on the following grid layers has just
                been reduced. Therefore, we add an additional grid with the
                same size as the previous one but for the reduced team. Then,
                copying the data from the domain of the larger team to the
                domain of the smaller team is easy. */

                /*
                if ( 0 == currentteam.myid() ) {
                    cout << "transfer level " <<
                        ((1<<(howmanylevels+1))-1)*factor_z << "×" <<
                        ((1<<(howmanylevels+1))-1)*factor_y << "×" <<
                        ((1<<(howmanylevels+1))-1)*factor_x <<
                        " distributed over " <<
                        localteamspec.num_units(0) << "×" <<
                        localteamspec.num_units(1) << "×" <<
                        localteamspec.num_units(2) << " units" << endl;
                }
                */

                levels.push_back(
                    new Level( *levels.back(),
                               ((1<<(howmanylevels+1))-1)*factor_z,
                               ((1<<(howmanylevels+1))-1)*factor_y,
                               ((1<<(howmanylevels+1))-1)*factor_x,
                               currentteam, localteamspec ) );
                initboundary_zero( *levels.back() );
            }

            //cout << "working unit " << dash::myid() << " / " << currentteam.myid() << " in subteam at position " << currentteam.position() << endl;

            /*
            if ( 0 == currentteam.myid() ) {
                cout << "compute level " <<
                    ((1<<(howmanylevels))-1)*factor_z << "×" <<
                    ((1<<(howmanylevels))-1)*factor_y << "×" <<
                    ((1<<(howmanylevels))-1)*factor_x <<
                    " distributed over " <<
                    localteamspec.num_units(0) << "×" <<
                    localteamspec.num_units(1) << "×" <<
                    localteamspec.num_units(2) << " units" << endl;
            }
            */

            /* do not try to allocate >= 8GB per core -- try to prevent myself
            from running too big a simulation on my laptop */
            assert( ((1<<(howmanylevels))-1)*factor_z *
                    ((1<<(howmanylevels))-1)*factor_y *
                    ((1<<(howmanylevels))-1)*factor_x < currentteam.size() * (1<<27) );

            levels.push_back(
                new Level( *levels.back(),
                           ((1<<(howmanylevels))-1)*factor_z ,
                           ((1<<(howmanylevels))-1)*factor_y ,
                           ((1<<(howmanylevels))-1)*factor_x ,
                           currentteam, localteamspec ) );

            initboundary_zero( *levels.back() );

        } else {

            //cout << "waiting unit " << dash::myid() << " / " << currentteam.myid() << " in subteam at position " << currentteam.position() << endl;

            /* this is a passive unit not taking part in the subteam that
            handles the coarser grids. insert a dummy entry in the vector
            of levels to signal that this is not the coarsest level globally. */
            levels.push_back( NULL );

            break;
        }

        --howmanylevels;
    }

    /* here all units and all teams meet again, those that were active for the coarsest
    levels and those that were dormant */
    dash::Team::All().barrier();

    /* Fill finest level. Strictly, we don't need to set any initial values here
    but we do it for demonstration in the graphical output */
    initgrid( *levels.front() );

    dash::Team::All().barrier();

    Allreduce res( dash::Team::All() );

    minimon.stop( "setup", dash::Team::All().size() );
/*
    if ( 0 == dash::myid()  ) {
        cout << "start v-cycle with res " << 0.1 << endl << endl;
    }
    v_cycle( levels.begin(), levels.end(), 2, 0.1, res );
    dash::Team::All().barrier();


    if ( 0 == dash::myid()  ) {
        cout << "start v-cycle with res " << 0.01 << endl << endl;
    }
    v_cycle( levels.begin(), levels.end(), 2, 0.01, res );
    dash::Team::All().barrier();
*/

    // algorithm
    minimon.start();

    if ( 0 == dash::myid()  ) {
        cout << "start w-cycle with res " << eps << endl;
    }
    //v_cycle( levels.begin(), levels.end(), 20, eps, res );
    recursive_cycle( levels.begin(), levels.end(), 20, 2 /* 2 for w cycle */, eps, res );

    dash::Team::All().barrier();

    if ( 0 == dash::myid()  ) {
        cout << "final smoothing with res " << eps << endl;
    }
    smoothen_final( *levels.front(), eps, res );

    minimon.stop( "algorithm", dash::Team::All().size() );

    dash::Team::All().barrier();

    if ( 0 == dash::myid() ) {

        if ( ! check_symmetry( *levels.front()->src_grid, eps ) ) {

            cout << "test for asymmetry of soution failed!" << endl;
        }
    }

    return res.get();
}


double do_simulation( uint32_t howmanylevels, double timerange, double timestep,
                      std::array< double, 3 >& dim ) {

    // setup
    minimon.start();

    TeamSpecT teamspec( dash::Team::All().size(), 1, 1 );
    teamspec.balance_extents();

    /* determine factors for width and height such that every unit has a power of two
    extent in every dimension and that the area is close to a square
    with aspect ratio \in [0,75,1,5] */

    uint32_t factor_z= 1;
    uint32_t factor_y= 1;
    uint32_t factor_x= 1;

    if ( 0 == dash::myid() ) {

        cout << "run simulation with " << dash::Team::All().size() << " units "
            "for grid of " <<
            ((1<<(howmanylevels))-1)*factor_z << "×" <<
            ((1<<(howmanylevels))-1)*factor_y << "×" <<
            ((1<<(howmanylevels))-1)*factor_x <<
            " for " << timerange << " seconds with output steps every " << timestep << " seconds " << endl;
    }

    /* physical dimensions 10m³ because it allows larger dt */
    Level* level= new Level( dim[0], dim[1], dim[2],
        ((1<<(howmanylevels))-1)*factor_z ,
        ((1<<(howmanylevels))-1)*factor_y ,
        ((1<<(howmanylevels))-1)*factor_x ,
        dash::Team::All(), teamspec );

    dash::barrier();

    initboundary( *level );

    initgrid( *level );

    dash::barrier();

    double dt= level->max_dt();

    Allreduce res( dash::Team::All() );

    minimon.stop( "setup", dash::Team::All().size() );

    // algorithm
    minimon.start();

    double time= 0.0;
    double timenext= time + timestep;
    uint32_t j= 0;

    if ( 0 == dash::myid() ) { cout << "t= " << time << " j= " << j << endl; }

    while ( time < timerange ) {

        while ( time + dt < timenext ) {

            smoothen( *level, res, dt );
            ++j;
            time += dt;
            // if ( 0 == dash::myid() ) { cout << "t= " << time << " dt= " << dt << endl; }
        }

        double shorten= ( timenext - time ) / dt;
         smoothen( *level, res, dt*shorten );
        ++j;

        time += timenext - time;
        timenext += timestep;

        if ( 0 == dash::myid() ) { cout << "t= " << time << " j= " << j << endl; }
    }


# if 0

    while ( res.get() > eps && j < 100000 ) {

        smoothen( *level, res );

        j++;

    }
    if ( 0 == dash::myid() ) {
        cout << "smoothing: " << j << " steps finest with residual " << res.get() << endl;
    }


#endif /* 0 */
    minimon.stop( "algorithm", dash::Team::All().size() );

    delete level;
    level= NULL;

    return res.get();
}


double do_flat_iteration( uint32_t howmanylevels, double eps, std::array< double, 3 >& dim ) {

    // setup
    minimon.start();

    TeamSpecT teamspec( dash::Team::All().size(), 1, 1 );
    teamspec.balance_extents();

    /* determine factors for width and height such that every unit has a power of two
    extent in every dimension and that the area is close to a square
    with aspect ratio \in [0,75,1,5] */

    uint32_t factor_z= 1;
    uint32_t factor_y= 1;
    uint32_t factor_x= 1;

    if ( 0 == dash::myid() ) {

        cout << "run flat iteration with " << dash::Team::All().size() << " units "
            "for grid of " <<
            ((1<<(howmanylevels))-1)*factor_z << "×" <<
            ((1<<(howmanylevels))-1)*factor_y << "×" <<
            ((1<<(howmanylevels))-1)*factor_x <<
            endl;
    }

    Level* level= new Level( dim[0], dim[1], dim[2],
        ((1<<(howmanylevels))-1)*factor_z ,
        ((1<<(howmanylevels))-1)*factor_y ,
        ((1<<(howmanylevels))-1)*factor_x ,
        dash::Team::All(), teamspec );

    dash::barrier();

    initboundary( *level );

    initgrid( *level );

    dash::barrier();

    Allreduce res( dash::Team::All() );

    minimon.stop( "setup", dash::Team::All().size() );

    // algorithm
    minimon.start();

    uint32_t j= 0;
    while ( res.get() > eps && j < 100000 ) {

        smoothen( *level, res );

        j++;
    }
    if ( 0 == dash::myid() ) {
        cout << "smoothing: " << j << " steps finest with residual " << res.get() << endl;
    }

    minimon.stop( "algorithm", dash::Team::All().size() );

    if ( 0 == dash::myid() ) {

        if ( ! check_symmetry( *level->src_grid, 0.01 ) ) {

            cout << "test for asymmetry of soution failed!" << endl;
        }
    }

    delete level;
    level= NULL;

    return res.get();
}


int main( int argc, char* argv[] ) {

    // main
    minimon.start();

    // dash::init
    minimon.start();
    dash::init(&argc, &argv);
    auto id= dash::myid();
    minimon.stop( "dash::init", dash::Team::All().size() );

    enum { FLAT, SIM, MULTIGRID, ELASTICMULTIGRID };

    int whattodo= MULTIGRID;

    uint32_t howmanylevels= 5;
    uint32_t howmanylevels_minimum= 2;
    double epsilon= 1.0e-3;
    double timerange= 10.0; /* 10 seconds */
    double timestep= 1.0/25.0; /* 25 FPS */

    /* physical dimensions of the simulation grid */
    std::array< double, 3 > dimensions= {10.0,10.0,10.0};

    /* round 1 over all command line arguments: check only for -h and --help */
    for ( int a= 1; a < argc; a++ ) {

        if ( 0 == strncmp( "-h", argv[a], 2  ) ||
                0 == strncmp( "--help", argv[a], 6 ) ) {

const char* HELPTEXT= "\n"
" <l>           number of levels to use at most, the simulation grid which is\n"
"               the finest grid in the multigrid hierarchy of grids will have\n"
"               2^l -1 inner elements plus 2 boundary elements per dimension\n"
"\n"
" Modes of operation\n"
"\n"
" -e[<s>]|--elastic[=<s>]\n"
"               use elastic multigrid mode, i.e., use fewer units (processes)\n"
"               on coarser grids, <s> gives the stepping for the unit reduction\n"
"               (default is every 3 levels a reduction of units)\n"
" -f|--flat     run flat mode, i.e., use iterative solver on a single grid\n"
" --sim <t> <s> run a simulation over time, that is also a \"flat\" solver\n"
"               working only on a single grid. It runs t seconds simulation\n"
"               time. The time step dt is determined by the grid and the\n"
"               stability condition. This mode matches all time steps n*s <= t\n"
"               exactly for the sake of a nice visualization.\n"
" \n"
" Further options\n"
"\n"
" --eps <eps>   define epsilon for the iterative solver in flat or multigrid modes,\n"
"               the iterative solver on any grid stops when residual <= eps\n"
" -d <d h w>    Set physical dimensions of the simulation grid in meters\n"
"               (default 10.0, 10.0, 10.0)\n"
"\n\n";

            if ( 0 == dash::myid() ) {

                cout << "\n"
                    " Call me as 'mpirun " << argv[0] << "' [-h|--help] [levels(default "<<howmanylevels<<")] "
                    "[...more options...]" <<
                    HELPTEXT;
            }

            dash::finalize();
            return 0;
        }
    }

    std::vector<std::string> tags;
    int split = 3;
    /* round 2 over all command line arguments */
    for ( int a= 1; a < argc; a++ ) {

        if ( 0 == strncmp( "--sim", argv[a], 5 ) && ( a+2 < argc ) ) {

            whattodo= SIM;
            timerange= atof( argv[a+1] );
            timestep= atof( argv[a+2] );
            a += 2;
            if ( 0 == dash::myid() ) {

                cout << "do simulation over " << timerange << " seconds with output "
                    "interval " << timestep << endl;
            }

        } else if ( 0 == strncmp( "-f", argv[a], 2  ) ||
                0 == strncmp( "--flat", argv[a], 6 )) {

            whattodo= FLAT;
            if ( 0 == dash::myid() ) {

                cout << "do flat iteration instead of multigrid" << endl;
            }

        } else if ( 0 == strcmp( "-e", argv[a] ) ||
                0 == strcmp( "--elastic", argv[a] )) {

            whattodo= ELASTICMULTIGRID;
            if ( 0 == dash::myid() ) {

                cout << "do multigrid iteration with changing number of units per grid" << endl;
            }

        } else if ( 0 == strncmp( "-e", argv[a], 2 ) ||
                0 == strncmp( "--elastic=", argv[a], 10 )) {

            whattodo= ELASTICMULTIGRID;
            if ( 0 == dash::myid() ) {

                cout << "do multigrid iteration with changing number of units per grid" << endl;
            }
            const char* split_arg = argv[a] + 2;
            if ( 0 == strncmp( "--elastic=", argv[a], 10 ) ) {
                split_arg = argv[a] + 10;
            }
            split = atoi(split_arg);

        } else if ( 0 == strncmp( "--eps", argv[a], 5  ) && ( a+1 < argc ) ) {

            epsilon= atof( argv[a+1] );
            a += 1;
            if ( 0 == dash::myid() ) {

                cout << "using epsilon " << epsilon << endl;
            }

        } else if ( 0 == strncmp( "-d", argv[a], 2  ) && ( a+3 < argc ) ) {

            dimensions[0]= atof( argv[a+1] );
            dimensions[1]= atof( argv[a+2] );
            dimensions[2]= atof( argv[a+3] );
            a += 3;
            if ( 0 == dash::myid() ) {

                cout << "using grid of dimensions " <<
                    dimensions[0] << "m×" <<
                    dimensions[1] << "m×" <<
                    dimensions[2] << "m" << endl;
            }

        } else {

            /* otherwise interpret as number of grid levels to employ */
            howmanylevels= atoi( argv[a] );
            if ( 0 == dash::myid() ) {
                cout << "using " << howmanylevels << " levels, " <<
                (1<<howmanylevels) << "³" << " per unit" << endl;
            }
        }
    }

    assert( howmanylevels > 2 );
    assert( howmanylevels <= 16 ); /* please adapt if you really want to go so high */

    double res = -1.0;
    switch ( whattodo ) {

        case SIM:
            tags.push_back("sim");
            tags.push_back("timerange=" + std::to_string(timerange));
            tags.push_back("timestep=" + std::to_string(timestep));
            res = do_simulation( howmanylevels, timerange, timestep, dimensions );
            break;
        case FLAT:
            tags.push_back("flat");
            tags.push_back("eps=" + std::to_string(epsilon));
            res = do_flat_iteration( howmanylevels, epsilon, dimensions );
            break;
        case ELASTICMULTIGRID:
            tags.push_back("multigridelastic");
            tags.push_back("eps=" + std::to_string(epsilon));
            tags.push_back("split=" + std::to_string(split));
            res = do_multigrid_elastic( howmanylevels, epsilon, dimensions, split );
            break;
        default:
            tags.push_back("multigrid");
            tags.push_back("eps=" + std::to_string(epsilon));
            res = do_multigrid_iteration( howmanylevels, epsilon, dimensions );
    }

    // dash::finalize
    minimon.start();

    dash::finalize();

    minimon.stop( "dash::finalize", dash::Team::All().size() );

    minimon.stop( "main", dash::Team::All().size() );
    minimon.print(id, tags);

    if ( id == 0 ) {
        cout << "\n"
             << "Result:\n"
             << "\n"
             << "Total runtime:         " << minimon.get("algorithm") << " sec\n"
             << "Included wait runtime: " << minimon.get("smoothen_wait") << " sec\n"
             << "Final residual:        " << res
             << endl;
    }

    return 0;
}
