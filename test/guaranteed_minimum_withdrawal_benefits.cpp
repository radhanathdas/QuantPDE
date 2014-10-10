////////////////////////////////////////////////////////////////////////////////
// guaranteed_minimum_withdrawal_benefit.cpp
// -----------------------------------------
//
// Computes the price of a Guaranteed Minimum Withdrawal Benefit (GMWB) using an
// implicit, impulse control formulation.
//
// Author: Parsiad Azimzadeh
////////////////////////////////////////////////////////////////////////////////

#include <QuantPDE/Core>
#include <QuantPDE/Modules/Lambdas>
#include <QuantPDE/Modules/Operators>

////////////////////////////////////////////////////////////////////////////////

#include <algorithm> // max, min
#include <iostream>  // cout
#include <numeric>   // accumulate
#include <tuple>     // get

////////////////////////////////////////////////////////////////////////////////

using namespace QuantPDE;
using namespace QuantPDE::Modules;

using namespace std;

////////////////////////////////////////////////////////////////////////////////

class Withdrawal final : public ControlledLinearSystem2 {

	static constexpr Real epsilon = 1e-12;

	RectilinearGrid2 &grid;
	Noncontrollable2 contractRate, kappa;

	Controllable2 control;

public:

	template <typename G, typename F1, typename F2>
	Withdrawal(G &grid, F1 &&contractRate, F2 &&kappa) noexcept :
		grid(grid),
		contractRate(contractRate),
		kappa(kappa),
		control( Control2(grid) )
	{
		registerControl( control );
	}

	virtual Matrix A(Real t) {
		Matrix M(grid.size(), grid.size());
		M.reserve(IntegerVector::Constant(grid.size(), 4));

		Index i = 0;
		for(auto node : grid) {
			const Real S = node[0]; // Investment
			const Real W = node[1]; // Withdrawal

			const Real lambda = control(t, S, W); // Control

			// Interpolation data
			auto data = interpolationData<2>(
				grid,
				{
					max(S - lambda * W, 0.),
					(1 - lambda) * W
				}
			);

			const Index i0 = get<0>( data[0] );
			const Index i1 = get<0>( data[1] );
			const Real  w0 = get<1>( data[0] );
			const Real  w1 = get<1>( data[1] );

			const Index j = grid.index(i0, i1);

			M.insert(i, j                     ) =    w0  *    w1 ;
			M.insert(i, j     + grid[0].size()) =    w0  * (1-w1);
			M.insert(i, j + 1                 ) = (1-w0) *    w1 ;
			M.insert(i, j + 1 + grid[0].size()) = (1-w0) * (1-w1);

			++i;
		}

		M.makeCompressed();
		return grid.identity() - M;
	}

	virtual Vector b(Real t) {
		Vector b( grid.vector() );
		for(auto node : accessor(grid, b)) {
			const Real S = (&node)[0]; // Investment
			const Real W = (&node)[1]; // Withdrawal

			//std::cout << S << ", " << W << ": ";

			// You have no money :(
			if(W <= epsilon) {
				*node = 0. - epsilon;
				//std::cout << "cashflow=" << *node << std::endl;
				continue;
			}

			// Control
			const Real lambda = control(t, S, W);

			// Contract rate of withdrawal
			const Real Gdt = contractRate(t, S, W);

			// Amount withdrawn, pre-penalty
			const Real lambdaW = lambda * W;

			// Withdrawal at no penalty
			if( lambda < min(Gdt / W, 1.) ) {
				*node = lambdaW - epsilon;
				//std::cout << "cashflow=" << *node << std::endl;
				continue;
			}

			// Withdrawal at a penalty
			*node = lambdaW - kappa(t, S, W) * (lambdaW - Gdt)
					- epsilon;
			//std::cout << "cashflow=" << *node << std::endl;

		}

		return b;
	}

};

////////////////////////////////////////////////////////////////////////////////

int main() {

	int n = 10;  // Optimal control partition size
	int N = 100; // Number of timesteps

	Real T = 10.;
	Real r = .05;
	Real v = .20;

	Real alpha = 0.;  // Hedging fee

	Real G = 10.;     // Contract rate
	Real kappa = 0.1; // Penalty rate

	int refinement = 2;

	////////////////////////////////////////////////////////////////////////
	// Solution grid
	////////////////////////////////////////////////////////////////////////

	RectilinearGrid2 grid(
		Axis {
			0., 5., 10., 15., 20., 25.,
			30., 35., 40., 45.,
			50., 55., 60., 65., 70., 72.5, 75., 77.5, 80., 82., 84.,
			86., 88., 90., 91., 92., 93., 94., 95.,
			96., 97., 98., 99., 100.,
			101., 102., 103., 104., 105., 106.,
			107., 108., 109., 110., 112., 114.,
			116., 118., 120., 123., 126.,
			130., 135., 140., 145., 150., 160., 175., 200., 225.,
			250., 300., 500., 750., 1000.
		},
		Axis::range(0., 2., 200.)
	);

	unsigned pow2l  = 1; // 2^l
	for(int l = 0; l < refinement; ++l) {

		////////////////////////////////////////////////////////////////////////
		// Control grid
		////////////////////////////////////////////////////////////////////////

		// Control partition 0 : 1/n : 1 (MATLAB notation)
		RectilinearGrid1 controls( Axis::range(0, 1. / (n * pow2l), 1) );

		////////////////////////////////////////////////////////////////////////
		// Iteration tree
		////////////////////////////////////////////////////////////////////////

		ReverseConstantStepper stepper(
			0.,              // Initial time
			T,               // Expiry time
			T / (N * pow2l)  // Timestep size
		);
		ToleranceIteration tolerance;
		stepper.setInnerIteration(tolerance);

		////////////////////////////////////////////////////////////////////////
		// Linear system tree
		////////////////////////////////////////////////////////////////////////

		BlackScholes<2, 0> bs(grid, r, v, alpha);
		ReverseLinearBDFTwo bdf(grid, bs);
		bdf.setIteration(stepper);

		Withdrawal impulse(grid, G * T / (N * pow2l), kappa);
		MinPolicyIteration2_1 policy(grid, controls, impulse);

		PenaltyMethod penalty(grid, bdf, policy);

		// TODO: It currently matters what order each linear system is
		//       associated with an iteration; fix this.

		penalty.setIteration(tolerance);
		policy.setIteration(tolerance);

		////////////////////////////////////////////////////////////////////////
		// Payoff
		////////////////////////////////////////////////////////////////////////

		Function2 payoff = [=] (Real S, Real W) {
			return max(S, (1 - kappa) * W);
		};

		////////////////////////////////////////////////////////////////////////
		// Running
		////////////////////////////////////////////////////////////////////////

		BiCGSTABSolver solver;

		auto V = stepper.solve(
			grid,    // Domain
			payoff,  // Initial condition
			penalty, // Root of linear system tree
			solver   // Linear system solver
		);

		////////////////////////////////////////////////////////////////////////
		// Print solution
		////////////////////////////////////////////////////////////////////////

		RectilinearGrid2 printGrid(
			Axis::range(0., 25., 200.),
			Axis::range(0., 25., 200.)
		);
		cout << accessor( printGrid, V );

		cout << endl;

		auto its = tolerance.iterations();
		Real inner = accumulate(its.begin(), its.end(), 0.)/its.size();

		cout << "average number of inner iterations: " << inner << endl;

		cout << endl;

		pow2l *= 2;

		////////////////////////////////////////////////////////////////////////
		// Refine Solution grid
		////////////////////////////////////////////////////////////////////////

		grid.refine( RectilinearGrid2::NewTickBetweenEachPair() );
	}

	return 0;
}
