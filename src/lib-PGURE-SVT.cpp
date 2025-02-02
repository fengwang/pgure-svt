/***************************************************************************

    PGURE-SVT Denoising

    Author: Tom Furnival
    Email:  tjof2@cam.ac.uk

    Copyright (C) 2015-16 Tom Furnival

    This program uses Singular Value Thresholding (SVT) [1], combined
    with an unbiased risk estimator (PGURE) to denoise a video sequence
    of microscopy images [2]. Noise parameters for a mixed Poisson-Gaussian
    noise model are automatically estimated during the denoising.

    References:
    [1] "Unbiased Risk Estimates for Singular Value Thresholding and
        Spectral Estimators", (2013), Candes, EJ et al.
        http://dx.doi.org/10.1109/TSP.2013.2270464

    [2] "An Unbiased Risk Estimator for Image Denoising in the Presence
        of Mixed Poisson–Gaussian Noise", (2014), Le Montagner, Y et al.
        http://dx.doi.org/10.1109/TIP.2014.2300821

    This file is part of PGURE-SVT.

    PGURE-SVT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    PGURE-SVT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PGURE-SVT. If not, see <http://www.gnu.org/licenses/>.

***************************************************************************/

// C++ headers
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <stdarg.h>
#include <stdexcept>
#include <stdlib.h>
#include <string>
#include <vector>

// OpenMP library
#include <omp.h>

// Armadillo library
#include <armadillo>

// Constant-time median filter
extern "C" {
	#include "medfilter.h"
}

// Own headers
#include "arps.hpp"
#include "hotpixel.hpp"
#include "params.hpp"
#include "noise.hpp"
#include "pgure.hpp"
#include "parallel.hpp"

// Little function to convert string "0"/"1" to boolean
bool strToBool(std::string const& s) {return s != "0";};

// Main program
extern "C" int PGURESVT(double *X,
                        double *Y,
                        int *dims,
                        int Bs,
                        int Bo,
                        int T,
                        bool pgureOpt,
                        double userLambda,
                        double alpha,
                        double mu,
                        double sigma,
                        int MotionP,
                        double tol,
                        int MedianSize,
                        double hotpixelthreshold,
                        int numthreads) {

	// Overall program timer
	auto overallstart = std::chrono::steady_clock::now();

	// Print program header
	std::cout<<std::endl;
	std::cout<<"PGURE-SVT Denoising"<<std::endl;
	std::cout<<"Author: Tom Furnival"<<std::endl;
	std::cout<<"Email:  tjof2@cam.ac.uk"<<std::endl<<std::endl;
	std::cout<<"Version 0.3.2 - May 2016"<<std::endl<<std::endl;

	// Set up OMP
	#if defined(_OPENMP)
		omp_set_dynamic(0);
		omp_set_num_threads(numthreads);
	#endif

  int NoiseMethod = 4;
  double lambda = (userLambda >= 0.) ? userLambda : 0.;

  int Nx = dims[0];
  int Ny = dims[1];
  int num_images = dims[2];

  // Copy the image sequence into the a cube
  arma::cube noisysequence(X, Nx, Ny, num_images);

  // Generate the clean and filtered sequences
  arma::cube cleansequence(Nx, Ny, num_images);
  arma::cube filteredsequence(Nx, Ny, num_images);

  cleansequence.zeros();
  filteredsequence.zeros();

  // Parameters for median filter
  int memsize = 512 * 1024;	// L2 cache size
  int filtsize = MedianSize;	// Median filter size in pixels

  // Perform the initial median filtering
  auto&& mfunc = [&]( int i )
  {
    unsigned short *Buffer = new unsigned short[Nx*Ny];
    unsigned short *FilteredBuffer = new unsigned short[Nx*Ny];
    arma::Mat<unsigned short> curslice = arma::conv_to<arma::Mat<unsigned short>>::from(noisysequence.slice(i).eval());
    inplace_trans(curslice);
    Buffer = curslice.memptr();
    ConstantTimeMedianFilter(Buffer,
                             FilteredBuffer,
                             Nx, Ny, Nx, Ny,
                             filtsize, 1, memsize);
		arma::Mat<unsigned short> filslice(FilteredBuffer, Nx, Ny);
		inplace_trans(filslice);
		filteredsequence.slice(i) = arma::conv_to<arma::mat>::from(filslice);
    delete[] Buffer;
    delete[] FilteredBuffer;
  };
  parallel( mfunc, static_cast<unsigned long long>(num_images) );

  /*
  for (int i = 0; i < num_images; i++) {
    arma::Mat<unsigned short> curslice = arma::conv_to<arma::Mat<unsigned short>>::from(noisysequence.slice(i).eval());
    inplace_trans(curslice);
    Buffer = curslice.memptr();
    ConstantTimeMedianFilter(Buffer,
                             FilteredBuffer,
                             Nx, Ny, Nx, Ny,
                             filtsize, 1, memsize);
		arma::Mat<unsigned short> filslice(FilteredBuffer, Nx, Ny);
		inplace_trans(filslice);
		filteredsequence.slice(i) = arma::conv_to<arma::mat>::from(filslice);
  }
  */


  // Initial outlier detection (for hot pixels)
  // using median absolute deviation
  HotPixelFilter(noisysequence, hotpixelthreshold);

	// Print table headings
	int ww = 10;
	std::cout<<std::endl;
	std::cout<<std::right<<std::setw(5*ww+5)<<std::string(5*ww+5,'-')<<std::endl;
	std::cout<<std::setw(5)<<"Frame"<<std::setw(ww)<<"Gain"<<std::setw(ww)<<"Offset"<<std::setw(ww)<<"Sigma"<<std::setw(ww)<<"Lambda"<<std::setw(ww)<<"Time (s)"<<std::endl;
	std::cout<<std::setw(5*ww+5)<<std::string(5*ww+5,'-')<<std::endl;

	// Loop over time windows
	int framewindow = std::floor(T/2);
	//for(int timeiter = 0; timeiter < num_images; timeiter++) {
    auto&& func = [&, lambda_=lambda]( int timeiter )
    {
		// Extract the subset of the image sequence
		arma::cube u(Nx, Ny, T), ufilter(Nx, Ny, T), v(Nx, Ny, T);
		if(timeiter < framewindow) {
			u = noisysequence.slices(0,2*framewindow);
			ufilter = filteredsequence.slices(0,2*framewindow);
		}
		else if(timeiter >= (num_images - framewindow)) {
			u = noisysequence.slices(num_images-2*framewindow-1,num_images-1);
			ufilter = filteredsequence.slices(num_images-2*framewindow-1,num_images-1);
		}
		else {
			u = noisysequence.slices(timeiter - framewindow, timeiter + framewindow);
			ufilter = filteredsequence.slices(timeiter - framewindow, timeiter + framewindow);
		}

		// Basic sequence normalization
		double inputmax = u.max();
		u /= inputmax;
		ufilter /= ufilter.max();

		// Perform noise estimation
		if(pgureOpt) {
		  NoiseEstimator *noise = new NoiseEstimator;
		  noise->Estimate(u,
		                  alpha,
		                  mu,
		                  sigma,
		                  4,
		                  NoiseMethod);
		  delete noise;
		}

		// Perform motion estimation
		MotionEstimator *motion = new MotionEstimator;
		motion->Estimate(ufilter,
		                 timeiter,
		                 framewindow,
		                 num_images,
		                 Bs,
		                 MotionP);
		arma::icube sequencePatches = motion->GetEstimate();
		delete motion;

		// Perform PGURE optimization
		PGURE *optimizer = new PGURE;
		optimizer->Initialize(u,
		                      sequencePatches,
		                      Bs,
		                      Bo,
		                      alpha,
		                      sigma,
		                      mu);
		// Determine optimum threshold value (max 1000 evaluations)
		if(pgureOpt) {
            auto lambda = lambda_;
			lambda = (timeiter == 0) ? arma::accu(u)/(Nx*Ny*T) : lambda;
			lambda = optimizer->Optimize(tol, lambda, u.max(), 1E3);
			v = optimizer->Reconstruct(lambda);
		}
		else {
			v = optimizer->Reconstruct(userLambda);
		}
		delete optimizer;

		// Rescale back to original range
		v *= inputmax;

		// Place frames back into sequence
		if(timeiter < framewindow) {
			cleansequence.slice(timeiter) = v.slice(timeiter);
		}
		else if(timeiter >= (num_images - framewindow)) {
			int endseqFrame = timeiter - (num_images - T);
			cleansequence.slice(timeiter) = v.slice(endseqFrame);
		}
		else {
			cleansequence.slice(timeiter) = v.slice(framewindow);
		}

	};
    parallel( func, static_cast<unsigned long long>(num_images) );

	// Finish the table off
	std::cout<<std::setw(5*ww+5)<<std::string(5*ww+5,'-')<<std::endl<<std::endl;

	// Overall program timer
	auto overallend = std::chrono::steady_clock::now();
	auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(overallend - overallstart);
	std::cout<<"Total time: "<<std::setprecision(5)<<(elapsed.count()/1E6)<<" seconds"<<std::endl<<std::endl;

  // Copy back to Python
  memcpy(Y, cleansequence.memptr(), cleansequence.n_elem*sizeof(double));

	return 0;
}
