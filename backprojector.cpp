/***************************************************************************
 *
 * Author: "Sjors H.W. Scheres"
 * MRC Laboratory of Molecular Biology
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * This complete copyright notice must be included in any revised version of the
 * source code. Additional authorship citations may be added, but existing
 * author citations must be preserved.
 ***************************************************************************/
/*
 * backprojector.cpp
 *
 *  Created on: 24 Aug 2010
 *      Author: scheres
 */


#include "backprojector.h"
#include "time.h"
#include "backproject_impl.h"
#include "fftw3.h"

#define TIMINGREC
#ifdef TIMINGREC
	#define RCTICREC(timer,label) (timer.tic(label))
    #define RCTOCREC(timer,label) (timer.toc(label))
	#define RCTIC(timer,label)
    #define RCTOC(timer,label)
#else
	#define RCTICREC(timer,label)
	#define RCTOCREC(timer,label)
	#define RCTIC(timer,label)
    #define RCTOC(timer,label)
#endif






void BackProjector::initialiseOnlyWeight(int current_size)
{
	// By default r_max is half ori_size
	if (current_size < 0)
		r_max = ori_size / 2;
	else
		r_max = current_size / 2;

	// Never allow r_max beyond Nyquist...
	r_max = XMIPP_MIN(r_max, ori_size / 2);

	// Set pad_size
	pad_size = 2 * (ROUND(padding_factor * r_max) + 1) + 1;

	// Short side of data array
	switch (ref_dim)
	{
	case 2:
	   weight.resize(pad_size, pad_size / 2 + 1);
	   break;
	case 3:
		weight.resize(pad_size, pad_size, pad_size / 2 + 1);
	   break;
	default:
	   REPORT_ERROR("Projector::resizeData%%ERROR: Dimension of the data array should be 2 or 3");
	}

	// Set origin in the y.z-center, but on the left side for x.
	weight.setXmippOrigin();
	weight.xinit=0;

}



void BackProjector::initialiseDataAndWeight(int current_size)
{

	initialiseData(current_size);
	weight.resize(data);
#ifdef COMGPU
	compweight.resize(compdatareal);
#endif

}

void BackProjector::rawinitialiseDataAndWeight(int current_size)
{

	rawinitialiseData(current_size);
	weight.resize(data);

}

void BackProjector::initialiseOnlyData(int current_size)
{

	initialiseData(current_size);
	data.initZeros();
	weight.initZeros();
#ifdef COMGPU
	compweight.initZeros();
	compdatareal.initZeros();
	compdataimag.initZeros();
#endif
}





void BackProjector::initZeros(int current_size)
{

	initialiseDataAndWeight(current_size);
	data.initZeros();
	weight.initZeros();
}

#ifdef BACKSLICE
void BackProjector::initslicedata(int current_size)
{

	if(pad_size == 0)
	{
		if (current_size < 0)
			r_max = ori_size / 2;
		else
			r_max = current_size / 2;

		// Never allow r_max beyond Nyquist...
		r_max = XMIPP_MIN(r_max, ori_size / 2);

		// Set pad_size
		pad_size = 2 * (ROUND(padding_factor * r_max) + 1) + 1;
	}
	int padx= pad_size /2 +1;
	int imgx = padx / 2;
	int imgy= (imgx-1)*2;
	int fullsize = imgx*imgy*8;

#ifdef PINMEM
	cudaHostAlloc((void **)&cpuindex, fullsize * sizeof(int), cudaHostAllocDefault);
	cudaHostAlloc((void **)&cpureal, fullsize * sizeof(RFLOAT), cudaHostAllocDefault);
	cudaHostAlloc((void **)&cpuimag, fullsize * sizeof(RFLOAT), cudaHostAllocDefault);
	cudaHostAlloc((void **)&cpuweight, fullsize * sizeof(RFLOAT), cudaHostAllocDefault);
#else

	cpuindex.resize(fullsize);
	cpureal.resize(fullsize);
	cpuimag.resize(fullsize);
	cpuweight.resize(fullsize);
	cpuindex.initZeros();
	cpureal.initZeros();
	cpuimag.initZeros();
	cpuweight.initZeros();
#endif

#ifdef FILTERSLICE
	int centerx= padx /2;
	int centery= pad_size/2;
	int centerz= pad_size/2;

	fxsize = 300;
	fysize = fxsize*2;
	fzsize = fxsize*2;
	fstartx=0;
	fendx=fxsize;
	fstarty = centery - fysize/2;
	fendy = fstarty+fysize -1;
	fstartz = centerz - fzsize /2;
	fendz = fstartz+fzsize -1 ;
	fxyzsize = fxsize * fysize * fzsize;

#endif

}
#endif

void BackProjector::backproject2Dto3D(const MultidimArray<Complex > &f2d,
		                        const Matrix2D<RFLOAT> &A, bool inv,
		                        const MultidimArray<RFLOAT> *Mweight,
								RFLOAT r_ewald_sphere, bool is_positive_curvature)
{
	RFLOAT fx, fy, fz, mfx, mfy, mfz, xp, yp, zp;
	int first_x, x0, x1, y0, y1, z0, z1, y, y2, r2;
	bool is_neg_x;
	RFLOAT dd000, dd001, dd010, dd011, dd100, dd101, dd110, dd111;
	Complex my_val;
	Matrix2D<RFLOAT> Ainv;
	RFLOAT my_weight = 1.;

	// f2d should already be in the right size (ori_size,orihalfdim)
    // AND the points outside max_r should already be zero...

	// Use the inverse matrix
    if (inv)
    	Ainv = A;
    else
    	Ainv = A.transpose();

    // Go from the 2D slice coordinates to the 3D coordinates
    Ainv *= (RFLOAT)padding_factor;  // take scaling into account directly
    int max_r2 = r_max * r_max;
    int min_r2_nn = r_min_nn * r_min_nn;

//#define DEBUG_BACKP
#ifdef DEBUG_BACKP
    std::cerr << " XSIZE(f2d)= "<< XSIZE(f2d) << std::endl;
    std::cerr << " YSIZE(f2d)= "<< YSIZE(f2d) << std::endl;
    std::cerr << " XSIZE(data)= "<< XSIZE(data) << std::endl;
    std::cerr << " YSIZE(data)= "<< YSIZE(data) << std::endl;
    std::cerr << " STARTINGX(data)= "<< STARTINGX(data) << std::endl;
    std::cerr << " STARTINGY(data)= "<< STARTINGY(data) << std::endl;
    std::cerr << " STARTINGZ(data)= "<< STARTINGZ(data) << std::endl;
    std::cerr << " max_r= "<< r_max << std::endl;
    std::cerr << " Ainv= " << Ainv << std::endl;
#endif

	// precalculate inverse of Ewald sphere diameter
    RFLOAT inv_diam_ewald = (r_ewald_sphere > 0.) ? 1./(2. * r_ewald_sphere) : 0.;
	if (!is_positive_curvature)
		inv_diam_ewald *= -1.;

    for (int i=0; i < YSIZE(f2d); i++)
	{
		// Dont search beyond square with side max_r
		if (i <= r_max)
		{
			y = i;
			first_x = 0;
		}
		else if (i >= YSIZE(f2d) - r_max)
		{
			y = i - YSIZE(f2d);
			// x==0 plane is stored twice in the FFTW format. Dont set it twice in BACKPROJECTION!
			first_x = 1;
		}
		else
			continue;

		y2 = y * y;
		for (int x=first_x; x <= r_max; x++)
		{
	    	// Only include points with radius < max_r (exclude points outside circle in square)
			r2 = x * x + y2;
			if (r2 > max_r2)
				continue;

			// Get the relevant value in the input image
			my_val = DIRECT_A2D_ELEM(f2d, i, x);

			// Get the weight
			if (Mweight != NULL)
				my_weight = DIRECT_A2D_ELEM(*Mweight, i, x);
			// else: my_weight was already initialised to 1.

			if (my_weight > 0.)
			{
				/*
				In our implementation, (x, y) are not scaled because:

	 			x_on_ewald = x * r / sqrt(x * x + y * y + r * r)
				           = x / sqrt(1 + (x * x + y * y) / (r * r))
				           ~ x * (1 - (x * x + y * y) / (2 * r * r) + O(1/r^4)) # binomial expansion
				           = x + O(1/r^2)

				same for y_on_ewald

	 			z_on_ewald = r - r * r / sqrt(x * x + y * y + r * r)
				           ~ r - r * (1 - (x * x + y * y) / (2 * r * r) + O(1/r^4)) # binomial expansion
				           = (x * x + y * y) / (2 * r) + O(1/r^3)

	                        The error is < 0.0005 reciprocal voxel even for extreme cases like 200kV, 1500 A particle, 1 A / pix.
				*/

				// Get logical coordinates in the 3D map
				RFLOAT z_on_ewaldp = inv_diam_ewald * (x * x	+ y * y);
				xp = Ainv(0,0) * x + Ainv(0,1) * y + Ainv(0,2) * z_on_ewaldp;
				yp = Ainv(1,0) * x + Ainv(1,1) * y + Ainv(1,2) * z_on_ewaldp;
				zp = Ainv(2,0) * x + Ainv(2,1) * y + Ainv(2,2) * z_on_ewaldp;

				if (interpolator == TRILINEAR || r2 < min_r2_nn)
				{

					// Only asymmetric half is stored
					if (xp < 0)
					{
						// Get complex conjugated hermitian symmetry pair
						xp = -xp;
						yp = -yp;
						zp = -zp;
						is_neg_x = true;
					}
					else
					{
						is_neg_x = false;
					}

					// Trilinear interpolation (with physical coords)
					// Subtract STARTINGY and STARTINGZ to accelerate access to data (STARTINGX=0)
					// In that way use DIRECT_A3D_ELEM, rather than A3D_ELEM
					x0 = FLOOR(xp);
					fx = xp - x0;
					x1 = x0 + 1;

					y0 = FLOOR(yp);
					fy = yp - y0;
					y0 -=  STARTINGY(data);
					y1 = y0 + 1;

					z0 = FLOOR(zp);
					fz = zp - z0;
					z0 -= STARTINGZ(data);
					z1 = z0 + 1;

					mfx = 1. - fx;
					mfy = 1. - fy;
					mfz = 1. - fz;

					dd000 = mfz * mfy * mfx;
					dd001 = mfz * mfy *  fx;
					dd010 = mfz *  fy * mfx;
					dd011 = mfz *  fy *  fx;
					dd100 =  fz * mfy * mfx;
					dd101 =  fz * mfy *  fx;
					dd110 =  fz *  fy * mfx;
					dd111 =  fz *  fy *  fx;

					if (is_neg_x)
						my_val = conj(my_val);

					// Store slice in 3D weighted sum
					DIRECT_A3D_ELEM(data, z0, y0, x0) += dd000 * my_val;
					DIRECT_A3D_ELEM(data, z0, y0, x1) += dd001 * my_val;
					DIRECT_A3D_ELEM(data, z0, y1, x0) += dd010 * my_val;
					DIRECT_A3D_ELEM(data, z0, y1, x1) += dd011 * my_val;
					DIRECT_A3D_ELEM(data, z1, y0, x0) += dd100 * my_val;
					DIRECT_A3D_ELEM(data, z1, y0, x1) += dd101 * my_val;
					DIRECT_A3D_ELEM(data, z1, y1, x0) += dd110 * my_val;
					DIRECT_A3D_ELEM(data, z1, y1, x1) += dd111 * my_val;
					// Store corresponding weights
					DIRECT_A3D_ELEM(weight, z0, y0, x0) += dd000 * my_weight;
					DIRECT_A3D_ELEM(weight, z0, y0, x1) += dd001 * my_weight;
					DIRECT_A3D_ELEM(weight, z0, y1, x0) += dd010 * my_weight;
					DIRECT_A3D_ELEM(weight, z0, y1, x1) += dd011 * my_weight;
					DIRECT_A3D_ELEM(weight, z1, y0, x0) += dd100 * my_weight;
					DIRECT_A3D_ELEM(weight, z1, y0, x1) += dd101 * my_weight;
					DIRECT_A3D_ELEM(weight, z1, y1, x0) += dd110 * my_weight;
					DIRECT_A3D_ELEM(weight, z1, y1, x1) += dd111 * my_weight;

				} // endif TRILINEAR
				else if (interpolator == NEAREST_NEIGHBOUR )
				{

					x0 = ROUND(xp);
					y0 = ROUND(yp);
					z0 = ROUND(zp);

					if (x0 < 0)
					{
						A3D_ELEM(data, -z0, -y0, -x0) += conj(my_val);
						A3D_ELEM(weight, -z0, -y0, -x0) += my_weight;
					}
					else
					{
						A3D_ELEM(data, z0, y0, x0) += my_val;
						A3D_ELEM(weight, z0, y0, x0) += my_weight;
					}

				} // endif NEAREST_NEIGHBOUR
				else
				{
					REPORT_ERROR("FourierInterpolator::backproject%%ERROR: unrecognized interpolator ");
				}
			} // endif weight>0.
		} // endif x-loop
	} // endif y-loop
}

void BackProjector::backproject1Dto2D(const MultidimArray<Complex > &f1d,
		                        const Matrix2D<RFLOAT> &A, bool inv,
		                        const MultidimArray<RFLOAT> *Mweight)
{

	RFLOAT fx, fy, mfx, mfy, xp, yp;
	int first_x, x0, x1, y0, y1, y, y2, r2;
	bool is_neg_x;
	RFLOAT dd00, dd01, dd10, dd11;
	Complex my_val;
	Matrix2D<RFLOAT> Ainv;
	RFLOAT my_weight = 1.;

	// f1d should already be in the right size (ori_size,orihalfdim)
    // AND the points outside max_r should already be zero...

	// Use the inverse matrix
    if (inv)
    	Ainv = A;
    else
    	Ainv = A.transpose();

    // Go from the 1D slice coordinates to the data-array coordinates
    Ainv *= (RFLOAT)padding_factor;  // take scaling into account directly

	for (int x=0; x <= r_max; x++)
	{
		// Get the relevant value in the input image
		my_val = DIRECT_A1D_ELEM(f1d, x);

		// Get the weight
		if (Mweight != NULL)
			my_weight = DIRECT_A1D_ELEM(*Mweight, x);
		// else: my_weight was already initialised to 1.

		if (my_weight > 0.)
		{
			// Get logical coordinates in the 3D map
			xp = Ainv(0,0) * x;
			yp = Ainv(1,0) * x;

			if (interpolator == TRILINEAR)
			{
				// Only asymmetric half is stored
				if (xp < 0)
				{
					// Get complex conjugated hermitian symmetry pair
					xp = -xp;
					yp = -yp;
					is_neg_x = true;
				}
				else
				{
					is_neg_x = false;
				}

				// Trilinear interpolation (with physical coords)
				// Subtract STARTINGY to accelerate access to data (STARTINGX=0)
				// In that way use DIRECT_A2D_ELEM, rather than A2D_ELEM
				x0 = FLOOR(xp);
				fx = xp - x0;
				x1 = x0 + 1;

				y0 = FLOOR(yp);
				fy = yp - y0;
				y0 -=  STARTINGY(data);
				y1 = y0 + 1;

				mfx = 1. - fx;
				mfy = 1. - fy;

				dd00 = mfy * mfx;
				dd01 = mfy *  fx;
				dd10 =  fy * mfx;
				dd11 =  fy *  fx;

				if (is_neg_x)
					my_val = conj(my_val);

				// Store slice in 3D weighted sum
				DIRECT_A2D_ELEM(data, y0, x0) += dd00 * my_val;
				DIRECT_A2D_ELEM(data, y0, x1) += dd01 * my_val;
				DIRECT_A2D_ELEM(data, y1, x0) += dd10 * my_val;
				DIRECT_A2D_ELEM(data, y1, x1) += dd11 * my_val;

				// Store corresponding weights
				DIRECT_A2D_ELEM(weight, y0, x0) += dd00 * my_weight;
				DIRECT_A2D_ELEM(weight, y0, x1) += dd01 * my_weight;
				DIRECT_A2D_ELEM(weight, y1, x0) += dd10 * my_weight;
				DIRECT_A2D_ELEM(weight, y1, x1) += dd11 * my_weight;

			} // endif TRILINEAR
			else if (interpolator == NEAREST_NEIGHBOUR )
			{
				x0 = ROUND(xp);
				y0 = ROUND(yp);
				if (x0 < 0)
				{
					A2D_ELEM(data, -y0, -x0) += conj(my_val);
					A2D_ELEM(weight, -y0, -x0) += my_weight;
				}
				else
				{
					A2D_ELEM(data, y0, x0) += my_val;
					A2D_ELEM(weight, y0, x0) += my_weight;
				}
			} // endif NEAREST_NEIGHBOUR
			else
			{
				REPORT_ERROR("FourierInterpolator::backproject1Dto2D%%ERROR: unrecognized interpolator ");
			}
		} // endif weight > 0.
	} // endif x-loop

}

void BackProjector::backrotate2D(const MultidimArray<Complex > &f2d,
		                         const Matrix2D<RFLOAT> &A, bool inv,
		                         const MultidimArray<RFLOAT> *Mweight)
{
	RFLOAT fx, fy, mfx, mfy, xp, yp;
	int first_x, x0, x1, y0, y1, y, y2, r2;
	bool is_neg_x;
	RFLOAT dd00, dd01, dd10, dd11;
	Complex my_val;
	Matrix2D<RFLOAT> Ainv;
	RFLOAT my_weight = 1.;

	// f2d should already be in the right size (ori_size,orihalfdim)
    // AND the points outside max_r should already be zero...

	// Use the inverse matrix
    if (inv)
    	Ainv = A;
    else
    	Ainv = A.transpose();

    // Go from the 2D slice coordinates to the data-array coordinates
    Ainv *= (RFLOAT)padding_factor;  // take scaling into account directly
    int max_r2 = r_max * r_max;
    int min_r2_nn = r_min_nn * r_min_nn;

//#define DEBUG_BACKROTATE
#ifdef DEBUG_BACKROTATE
    std::cerr << " XSIZE(f2d)= "<< XSIZE(f2d) << std::endl;
    std::cerr << " YSIZE(f2d)= "<< YSIZE(f2d) << std::endl;
    std::cerr << " XSIZE(data)= "<< XSIZE(data) << std::endl;
    std::cerr << " YSIZE(data)= "<< YSIZE(data) << std::endl;
    std::cerr << " STARTINGX(data)= "<< STARTINGX(data) << std::endl;
    std::cerr << " STARTINGY(data)= "<< STARTINGY(data) << std::endl;
    std::cerr << " STARTINGZ(data)= "<< STARTINGZ(data) << std::endl;
    std::cerr << " max_r= "<< r_max << std::endl;
    std::cerr << " Ainv= " << Ainv << std::endl;
#endif

    for (int i=0; i < YSIZE(f2d); i++)
	{
		// Don't search beyond square with side max_r
		if (i <= r_max)
		{
			y = i;
			first_x = 0;
		}
		else if (i >= YSIZE(f2d) - r_max)
		{
			y = i - YSIZE(f2d);
			// x==0 plane is stored twice in the FFTW format. Dont set it twice in BACKPROJECTION!
			first_x = 1;
		}
		else
			continue;

		y2 = y * y;
		for (int x=first_x; x <= r_max; x++)
		{
	    	// Only include points with radius < max_r (exclude points outside circle in square)
			r2 = x * x + y2;
			if (r2 > max_r2)
				continue;

			// Get the relevant value in the input image
			my_val = DIRECT_A2D_ELEM(f2d, i, x);

			// Get the weight
			if (Mweight != NULL)
				my_weight = DIRECT_A2D_ELEM(*Mweight, i, x);
			// else: my_weight was already initialised to 1.

			if (my_weight > 0.)
			{
				// Get logical coordinates in the 3D map
				xp = Ainv(0,0) * x + Ainv(0,1) * y;
				yp = Ainv(1,0) * x + Ainv(1,1) * y;

				if (interpolator == TRILINEAR || r2 < min_r2_nn)
				{
					// Only asymmetric half is stored
					if (xp < 0)
					{
						// Get complex conjugated hermitian symmetry pair
						xp = -xp;
						yp = -yp;
						is_neg_x = true;
					}
					else
					{
						is_neg_x = false;
					}

					// Trilinear interpolation (with physical coords)
					// Subtract STARTINGY to accelerate access to data (STARTINGX=0)
					// In that way use DIRECT_A2D_ELEM, rather than A2D_ELEM
					x0 = FLOOR(xp);
					fx = xp - x0;
					x1 = x0 + 1;

					y0 = FLOOR(yp);
					fy = yp - y0;
					y0 -=  STARTINGY(data);
					y1 = y0 + 1;

					mfx = 1. - fx;
					mfy = 1. - fy;

					dd00 = mfy * mfx;
					dd01 = mfy *  fx;
					dd10 =  fy * mfx;
					dd11 =  fy *  fx;

					if (is_neg_x)
						my_val = conj(my_val);

					// Store slice in 3D weighted sum
					DIRECT_A2D_ELEM(data, y0, x0) += dd00 * my_val;
					DIRECT_A2D_ELEM(data, y0, x1) += dd01 * my_val;
					DIRECT_A2D_ELEM(data, y1, x0) += dd10 * my_val;
					DIRECT_A2D_ELEM(data, y1, x1) += dd11 * my_val;

					// Store corresponding weights
					DIRECT_A2D_ELEM(weight, y0, x0) += dd00 * my_weight;
					DIRECT_A2D_ELEM(weight, y0, x1) += dd01 * my_weight;
					DIRECT_A2D_ELEM(weight, y1, x0) += dd10 * my_weight;
					DIRECT_A2D_ELEM(weight, y1, x1) += dd11 * my_weight;

				} // endif TRILINEAR
				else if (interpolator == NEAREST_NEIGHBOUR )
				{
					x0 = ROUND(xp);
					y0 = ROUND(yp);
					if (x0 < 0)
					{
						A2D_ELEM(data, -y0, -x0) += conj(my_val);
						A2D_ELEM(weight, -y0, -x0) += my_weight;
					}
					else
					{
						A2D_ELEM(data, y0, x0) += my_val;
						A2D_ELEM(weight, y0, x0) += my_weight;
					}
				} // endif NEAREST_NEIGHBOUR
				else
				{
					REPORT_ERROR("FourierInterpolator::backrotate2D%%ERROR: unrecognized interpolator ");
				}
			} // endif weight > 0.
		} // endif x-loop
	} // endif y-loop
}

void BackProjector::backrotate3D(const MultidimArray<Complex > &f3d,
		                         const Matrix2D<RFLOAT> &A, bool inv,
		                         const MultidimArray<RFLOAT> *Mweight)
{
	RFLOAT fx, fy, fz, mfx, mfy, mfz, xp, yp, zp;
	int first_x, x0, x1, y0, y1, z0, z1, y, y2, z, z2, r2;
	bool is_neg_x;
	RFLOAT dd000, dd010, dd100, dd110, dd001, dd011, dd101, dd111;
	Complex my_val;
	Matrix2D<RFLOAT> Ainv;
	RFLOAT my_weight = 1.;

	// f3d should already be in the right size (ori_size,orihalfdim)
    // AND the points outside max_r should already be zero...

	// Use the inverse matrix
    if (inv)
    	Ainv = A;
    else
    	Ainv = A.transpose();

    // Go from the 2D slice coordinates to the data-array coordinates
    Ainv *= (RFLOAT)padding_factor;  // take scaling into account directly
    int max_r2 = r_max * r_max;
    int min_r2_nn = r_min_nn * r_min_nn;

//#define DEBUG_BACKROTATE
#ifdef DEBUG_BACKROTATE
    std::cerr << " XSIZE(f3d)= "<< XSIZE(f3d) << std::endl;
    std::cerr << " YSIZE(f3d)= "<< YSIZE(f3d) << std::endl;
    std::cerr << " XSIZE(data)= "<< XSIZE(data) << std::endl;
    std::cerr << " YSIZE(data)= "<< YSIZE(data) << std::endl;
    std::cerr << " STARTINGX(data)= "<< STARTINGX(data) << std::endl;
    std::cerr << " STARTINGY(data)= "<< STARTINGY(data) << std::endl;
    std::cerr << " STARTINGZ(data)= "<< STARTINGZ(data) << std::endl;
    std::cerr << " max_r= "<< r_max << std::endl;
    std::cerr << " Ainv= " << Ainv << std::endl;
#endif

    for (int k=0; k < ZSIZE(f3d); k++)
	{
		// Don't search beyond square with side max_r
		if (k <= r_max)
		{
			z = k;
			first_x = 0;
		}
		else if (k >= YSIZE(f3d) - r_max)
		{
			z = k - YSIZE(f3d);
			/// TODO: still check this better in the 3D case!!!
			// x==0 (y,z)-plane is stored twice in the FFTW format. Don't set it twice in BACKPROJECTION!
			first_x = 1;
		}
		else
			continue;

		z2 = z * z;
		for (int i=0; i < YSIZE(f3d); i++)
		{
			// Don't search beyond square with side max_r
			if (i <= r_max)
			{
				y = i;
			}
			else if (i >= YSIZE(f3d) - r_max)
			{
				y = i - YSIZE(f3d);
			}
			else
				continue;

			y2 = y * y;
			for (int x = first_x; x <= r_max; x++)
			{
				// Only include points with radius < max_r (exclude points outside circle in square)
				r2 = x * x + y2 + z2;
				if (r2 > max_r2)
					continue;

				// Get the relevant value in the input image
				my_val = DIRECT_A3D_ELEM(f3d, k, i, x);

				// Get the weight
				if (Mweight != NULL)
					my_weight = DIRECT_A3D_ELEM(*Mweight, k, i, x);
				// else: my_weight was already initialised to 1.

				if (my_weight > 0.)
				{
					// Get logical coordinates in the 3D map
					xp = Ainv(0,0) * x + Ainv(0,1) * y + Ainv(0,2) * z;
					yp = Ainv(1,0) * x + Ainv(1,1) * y + Ainv(1,2) * z;
					zp = Ainv(2,0) * x + Ainv(2,1) * y + Ainv(2,2) * z;

					if (interpolator == TRILINEAR || r2 < min_r2_nn)
					{
						// Only asymmetric half is stored
						if (xp < 0)
						{
							// Get complex conjugated hermitian symmetry pair
							xp = -xp;
							yp = -yp;
							zp = -zp;
							is_neg_x = true;
						}
						else
						{
							is_neg_x = false;
						}

						// Trilinear interpolation (with physical coords)
						// Subtract STARTINGY to accelerate access to data (STARTINGX=0)
						// In that way use DIRECT_A3D_ELEM, rather than A3D_ELEM
						x0 = FLOOR(xp);
						fx = xp - x0;
						x1 = x0 + 1;

						y0 = FLOOR(yp);
						fy = yp - y0;
						y0 -=  STARTINGY(data);
						y1 = y0 + 1;

						z0 = FLOOR(zp);
						fz = zp - z0;
						z0 -=  STARTINGZ(data);
						z1 = z0 + 1;

						mfx = 1. - fx;
						mfy = 1. - fy;
						mfz = 1. - fz;

						dd000 = mfz * mfy * mfx;
						dd001 = mfz * mfy *  fx;
						dd010 = mfz *  fy * mfx;
						dd011 = mfz *  fy *  fx;
						dd100 =  fz * mfy * mfx;
						dd101 =  fz * mfy *  fx;
						dd110 =  fz *  fy * mfx;
						dd111 =  fz *  fy *  fx;

						if (is_neg_x)
							my_val = conj(my_val);

						// Store slice in 3D weighted sum
						DIRECT_A3D_ELEM(data, z0, y0, x0) += dd000 * my_val;
						DIRECT_A3D_ELEM(data, z0, y0, x1) += dd001 * my_val;
						DIRECT_A3D_ELEM(data, z0, y1, x0) += dd010 * my_val;
						DIRECT_A3D_ELEM(data, z0, y1, x1) += dd011 * my_val;
						DIRECT_A3D_ELEM(data, z1, y0, x0) += dd100 * my_val;
						DIRECT_A3D_ELEM(data, z1, y0, x1) += dd101 * my_val;
						DIRECT_A3D_ELEM(data, z1, y1, x0) += dd110 * my_val;
						DIRECT_A3D_ELEM(data, z1, y1, x1) += dd111 * my_val;
						// Store corresponding weights
						DIRECT_A3D_ELEM(weight, z0, y0, x0) += dd000 * my_weight;
						DIRECT_A3D_ELEM(weight, z0, y0, x1) += dd001 * my_weight;
						DIRECT_A3D_ELEM(weight, z0, y1, x0) += dd010 * my_weight;
						DIRECT_A3D_ELEM(weight, z0, y1, x1) += dd011 * my_weight;
						DIRECT_A3D_ELEM(weight, z1, y0, x0) += dd100 * my_weight;
						DIRECT_A3D_ELEM(weight, z1, y0, x1) += dd101 * my_weight;
						DIRECT_A3D_ELEM(weight, z1, y1, x0) += dd110 * my_weight;
						DIRECT_A3D_ELEM(weight, z1, y1, x1) += dd111 * my_weight;


					} // endif TRILINEAR
					else if (interpolator == NEAREST_NEIGHBOUR )
					{
						x0 = ROUND(xp);
						y0 = ROUND(yp);
						z0 = ROUND(zp);

						if (x0 < 0)
						{
							A3D_ELEM(data, -z0, -y0, -x0) += conj(my_val);
							A3D_ELEM(weight, -z0, -y0, -x0) += my_weight;
						}
						else
						{
							A3D_ELEM(data, z0, y0, x0) += my_val;
							A3D_ELEM(weight, z0, y0, x0) += my_weight;
						}

					} // endif NEAREST_NEIGHBOUR
					else
					{
						REPORT_ERROR("BackProjector::backrotate3D%%ERROR: unrecognized interpolator ");
					}
				} // endif weight > 0.
			} // endif x-loop
		} // endif y-loop
	} // endif z-loop
}

void BackProjector::getLowResDataAndWeight(MultidimArray<Complex > &lowres_data, MultidimArray<RFLOAT> &lowres_weight,
		int lowres_r_max)
{

	int lowres_r2_max = ROUND(padding_factor * lowres_r_max) * ROUND(padding_factor * lowres_r_max);
	int lowres_pad_size = 2 * (ROUND(padding_factor * lowres_r_max) + 1) + 1;

	// Check lowres_r_max is not too big
	if (lowres_r_max > r_max)
		REPORT_ERROR("BackProjector::getLowResDataAndWeight%%ERROR: lowres_r_max is bigger than r_max");

	// Initialize lowres_data and low_res_weight arrays
	lowres_data.clear();
	lowres_weight.clear();

	if (ref_dim == 2)
	{
		lowres_data.resize(lowres_pad_size, lowres_pad_size / 2 + 1);
		lowres_weight.resize(lowres_pad_size, lowres_pad_size / 2 + 1);
	}
	else
	{
		lowres_data.resize(lowres_pad_size, lowres_pad_size, lowres_pad_size / 2 + 1);
		lowres_weight.resize(lowres_pad_size, lowres_pad_size, lowres_pad_size / 2 + 1);
	}
	lowres_data.setXmippOrigin();
	lowres_data.xinit=0;
	lowres_weight.setXmippOrigin();
	lowres_weight.xinit=0;

	// fill lowres arrays with relevant values
	FOR_ALL_ELEMENTS_IN_ARRAY3D(lowres_data)
	{
		if (k*k + i*i + j*j <= lowres_r2_max)
		{
			A3D_ELEM(lowres_data, k, i, j) = A3D_ELEM(data, k , i, j);
			A3D_ELEM(lowres_weight, k, i, j) = A3D_ELEM(weight, k , i, j);
		}
	}
}

void BackProjector::setLowResDataAndWeight(MultidimArray<Complex > &lowres_data, MultidimArray<RFLOAT> &lowres_weight,
		int lowres_r_max)
{

	int lowres_r2_max = ROUND(padding_factor * lowres_r_max) * ROUND(padding_factor * lowres_r_max);
	int lowres_pad_size = 2 * (ROUND(padding_factor * lowres_r_max) + 1) + 1;

	// Check lowres_r_max is not too big
	if (lowres_r_max > r_max)
		REPORT_ERROR("BackProjector::getLowResDataAndWeight%%ERROR: lowres_r_max is bigger than r_max");

	// Check sizes of lowres_data and lowres_weight
	if (YSIZE(lowres_data) != lowres_pad_size || XSIZE(lowres_data) != lowres_pad_size / 2 + 1 ||
			(ref_dim ==3 && ZSIZE(lowres_data) != lowres_pad_size) )
		REPORT_ERROR("BackProjector::setLowResDataAndWeight%%ERROR: lowres_data is not of expected size...");
	if (YSIZE(lowres_weight) != lowres_pad_size || XSIZE(lowres_weight) != lowres_pad_size / 2 + 1 ||
			(ref_dim ==3 && ZSIZE(lowres_weight) != lowres_pad_size) )
		REPORT_ERROR("BackProjector::setLowResDataAndWeight%%ERROR: lowres_weight is not of expected size...");

	// Re-set origin to the expected place
	lowres_data.setXmippOrigin();
	lowres_data.xinit=0;
	lowres_weight.setXmippOrigin();
	lowres_weight.xinit=0;

	// Overwrite data and weight with the lowres arrays
	FOR_ALL_ELEMENTS_IN_ARRAY3D(lowres_data)
	{
		if (k*k + i*i + j*j <= lowres_r2_max)
		{
			A3D_ELEM(data, k, i, j) = A3D_ELEM(lowres_data, k , i, j);
			A3D_ELEM(weight, k, i, j) = A3D_ELEM(lowres_weight, k , i, j);
		}
	}
}

void BackProjector::getDownsampledAverage(MultidimArray<Complex>& avg, bool divide) const
{
    MultidimArray<RFLOAT> down_weight;

    // Pre-set down_data and down_weight sizes
    int down_size = 2 * (r_max + 1) + 1;

    // Short side of data array
    switch (ref_dim)
    {
    case 2:
       avg.initZeros(down_size, down_size / 2 + 1);
       break;
    case 3:
       avg.initZeros(down_size, down_size, down_size / 2 + 1);
       break;
    default:
       REPORT_ERROR("BackProjector::getDownsampledAverage%%ERROR: Dimension of the data array should be 2 or 3");
    }
    // Set origin in the y.z-center, but on the left side for x.
    avg.setXmippOrigin();
    avg.xinit=0;
    // Resize down_weight the same as down_data
    down_weight.initZeros(avg);

    // Now calculate the down-sized sum
    int kp, ip, jp;
    FOR_ALL_ELEMENTS_IN_ARRAY3D(data)
    {
        kp = ROUND((RFLOAT)k/padding_factor);
        ip = ROUND((RFLOAT)i/padding_factor);
        jp = ROUND((RFLOAT)j/padding_factor);

// TMP
//#define CHECK_SIZE
#ifdef CHECK_SIZE
        if (kp > FINISHINGZ(avg) || ip > FINISHINGY(avg) || jp > FINISHINGX(avg) ||
                kp < STARTINGZ(avg) || ip < STARTINGY(avg) || jp < STARTINGX(avg))
        {
            std::cerr << " kp= " << kp << " ip= " << ip << " jp= " << jp << std::endl;
            avg.printShape();
            REPORT_ERROR("BackProjector::getDownsampledAverage: indices out of range");
        }
#endif
        A3D_ELEM(avg, kp, ip, jp) += A3D_ELEM(data, k , i, j);
        A3D_ELEM(down_weight, kp, ip, jp) += (divide? A3D_ELEM(weight, k , i, j) : 1.0);
    }

    // Calculate the straightforward average in the downsampled arrays
    FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(avg)
    {
        if (DIRECT_MULTIDIM_ELEM(down_weight, n) > 0.)
        {
            DIRECT_MULTIDIM_ELEM(avg, n) /= DIRECT_MULTIDIM_ELEM(down_weight, n);
        }
        else
        {
            DIRECT_MULTIDIM_ELEM(avg, n) = 0.;
        }
    }
}


void BackProjector::calculateDownSampledFourierShellCorrelation(const MultidimArray<Complex>& avg1,
                                                                const MultidimArray<Complex>& avg2,
                                                                MultidimArray<RFLOAT>& fsc) const
{

    if (!avg1.sameShape(avg2))
        REPORT_ERROR("ERROR BackProjector::calculateDownSampledFourierShellCorrelation: two arrays have different sizes");

    MultidimArray<RFLOAT> num, den1, den2;
    num.initZeros(ori_size/2 + 1);
    den1.initZeros(num);
    den2.initZeros(num);
    fsc.initZeros(num);

    FOR_ALL_ELEMENTS_IN_ARRAY3D(avg1)
    {
        RFLOAT R = sqrt(k*k + i*i + j*j);

        if (R > r_max) continue;

        int idx = ROUND(R);

        Complex z1 = A3D_ELEM(avg1, k, i, j);
        Complex z2 = A3D_ELEM(avg2, k, i, j);

        RFLOAT nrmz1 = z1.norm();
        RFLOAT nrmz2 = z2.norm();

        num(idx) += z1.real * z2.real + z1.imag * z2.imag;
        den1(idx) += nrmz1;
        den2(idx) += nrmz2;
    }

    FOR_ALL_ELEMENTS_IN_ARRAY1D(fsc)
    {
        if (den1(i)*den2(i) > 0.)
        {
            fsc(i) = num(i)/sqrt(den1(i)*den2(i));
        }
    }

    // Always set zero-resolution shell to FSC=1
    // Raimond Ravelli reported a problem with FSC=1 at res=0 on 13feb2013...
    // (because of a suboptimal normalisation scheme, but anyway)
    fsc(0) = 1.;
}

void BackProjector::reconstruct(MultidimArray<RFLOAT> &vol_out,
                                int max_iter_preweight,
                                bool do_map,
                                RFLOAT tau2_fudge,
                                MultidimArray<RFLOAT> &tau2_io, // can be input/output
                                MultidimArray<RFLOAT> &sigma2_out,
                                MultidimArray<RFLOAT> &data_vs_prior_out,
                                MultidimArray<RFLOAT> &fourier_coverage_out,
                                const MultidimArray<RFLOAT> &fsc, // only input
                                RFLOAT normalise,
                                bool update_tau2_with_fsc,
                                bool is_whole_instead_of_half,
                                int nr_threads,
                                int minres_map,
                                bool printTimes,
								bool do_fsc0999)
{
	Timer ReconTimer;

#ifdef TIMINGREC
	printTimes=1;
	int ReconS_1 = ReconTimer.setNew(" RcS1_Init ");
	int ReconS_2 = ReconTimer.setNew(" RcS2_Shape&Noise ");
	int ReconS_2_5 = ReconTimer.setNew(" RcS2.5_Regularize ");
	int ReconS_3 = ReconTimer.setNew(" RcS3_skipGridding ");
	int ReconS_4 = ReconTimer.setNew(" RcS4_doGridding_norm ");
	int ReconS_5 = ReconTimer.setNew(" RcS5_doGridding_init ");
	int ReconS_6 = ReconTimer.setNew(" RcS6_doGridding_iter ");
	int ReconS_7 = ReconTimer.setNew(" RcS7_doGridding_apply ");
	int ReconS_8 = ReconTimer.setNew(" RcS8_blobConvolute ");
	int ReconS_9 = ReconTimer.setNew(" RcS9_blobResize ");
	int ReconS_10 = ReconTimer.setNew(" RcS10_blobSetReal ");
	int ReconS_11 = ReconTimer.setNew(" RcS11_blobSetTemp ");
	int ReconS_12 = ReconTimer.setNew(" RcS12_blobTransform ");
	int ReconS_13 = ReconTimer.setNew(" RcS13_blobCenterFFT ");
	int ReconS_14 = ReconTimer.setNew(" RcS14_blobNorm1 ");
	int ReconS_15 = ReconTimer.setNew(" RcS15_blobSoftMask ");
	int ReconS_16 = ReconTimer.setNew(" RcS16_blobNorm2 ");
	int ReconS_17 = ReconTimer.setNew(" RcS17_WindowReal ");
	int ReconS_18 = ReconTimer.setNew(" RcS18_GriddingCorrect ");
	int ReconS_19 = ReconTimer.setNew(" RcS19_tauInit ");
	int ReconS_20 = ReconTimer.setNew(" RcS20_tausetReal ");
	int ReconS_21 = ReconTimer.setNew(" RcS21_tauTransform ");
	int ReconS_22 = ReconTimer.setNew(" RcS22_tautauRest ");
	int ReconS_23 = ReconTimer.setNew(" RcS23_tauShrinkToFit ");
	int ReconS_24 = ReconTimer.setNew(" RcS24_extra ");
#endif

	//printf("At beginning : %f %f \n",weight.data[0],weight.data[1]);
    // never rely on references (handed to you from the outside) for computation:
    // they could be the same (i.e. reconstruct(..., dummy, dummy, dummy, dummy, ...); )
    MultidimArray<RFLOAT> sigma2, data_vs_prior, fourier_coverage;
	MultidimArray<RFLOAT> tau2 = tau2_io;


    RCTICREC(ReconTimer,ReconS_1);
    FourierTransformer transformer;
	MultidimArray<RFLOAT> Fweight;
	// Fnewweight can become too large for a float: always keep this one in double-precision
	MultidimArray<double> Fnewweight;
	MultidimArray<Complex>& Fconv = transformer.getFourierReference();
	int max_r2 = ROUND(r_max * padding_factor) * ROUND(r_max * padding_factor);

//#define DEBUG_RECONSTRUCT
#ifdef DEBUG_RECONSTRUCT
	Image<RFLOAT> ttt;
	FileName fnttt;
	ttt()=weight;
	ttt.write("reconstruct_initial_weight.spi");
	std::cerr << " pad_size= " << pad_size << " padding_factor= " << padding_factor << " max_r2= " << max_r2 << std::endl;
#endif

    // Set Fweight, Fnewweight and Fconv to the right size
    if (ref_dim == 2)
        vol_out.setDimensions(pad_size, pad_size, 1, 1);
    else
        // Too costly to actually allocate the space
        // Trick transformer with the right dimensions
        vol_out.setDimensions(pad_size, pad_size, pad_size, 1);

    transformer.setReal(vol_out); // Fake set real. 1. Allocate space for Fconv 2. calculate plans.
    vol_out.clear(); // Reset dimensions to 0

    RCTOCREC(ReconTimer,ReconS_1);
    RCTICREC(ReconTimer,ReconS_2);

    Fweight.reshape(Fconv);
    if (!skip_gridding)
    	Fnewweight.reshape(Fconv);

	// Go from projector-centered to FFTW-uncentered
	decenter(weight, Fweight, max_r2);

	// Take oversampling into account
	RFLOAT oversampling_correction = (ref_dim == 3) ? (padding_factor * padding_factor * padding_factor) : (padding_factor * padding_factor);
	MultidimArray<RFLOAT> counter;

	// First calculate the radial average of the (inverse of the) power of the noise in the reconstruction
	// This is the left-hand side term in the nominator of the Wiener-filter-like update formula
	// and it is stored inside the weight vector
	// Then, if (do_map) add the inverse of tau2-spectrum values to the weight
	sigma2.initZeros(ori_size/2 + 1);
	counter.initZeros(ori_size/2 + 1);
	FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
	{
		int r2 = kp * kp + ip * ip + jp * jp;
		if (r2 < max_r2)
		{
			int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
			RFLOAT invw = oversampling_correction * DIRECT_A3D_ELEM(Fweight, k, i, j);
			DIRECT_A1D_ELEM(sigma2, ires) += invw;
			DIRECT_A1D_ELEM(counter, ires) += 1.;
		}
    }

	// Average (inverse of) sigma2 in reconstruction
	FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(sigma2)
	{
        if (DIRECT_A1D_ELEM(sigma2, i) > 1e-10)
            DIRECT_A1D_ELEM(sigma2, i) = DIRECT_A1D_ELEM(counter, i) / DIRECT_A1D_ELEM(sigma2, i);
        else if (DIRECT_A1D_ELEM(sigma2, i) == 0)
            DIRECT_A1D_ELEM(sigma2, i) = 0.;
		else
		{
			std::cerr << " DIRECT_A1D_ELEM(sigma2, i)= " << DIRECT_A1D_ELEM(sigma2, i) << std::endl;
			REPORT_ERROR("BackProjector::reconstruct: ERROR: unexpectedly small, yet non-zero sigma2 value, this should not happen...a");
        }
    }

	if (update_tau2_with_fsc)
    {
        tau2.reshape(ori_size/2 + 1);
        data_vs_prior.initZeros(ori_size/2 + 1);
		// Then calculate new tau2 values, based on the FSC
		if (!fsc.sameShape(sigma2) || !fsc.sameShape(tau2))
		{
			fsc.printShape(std::cerr);
			tau2.printShape(std::cerr);
			sigma2.printShape(std::cerr);
			REPORT_ERROR("ERROR BackProjector::reconstruct: sigma2, tau2 and fsc have different sizes");
		}
		FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(sigma2)
        {
			// FSC cannot be negative or zero for conversion into tau2
			RFLOAT myfsc = XMIPP_MAX(0.001, DIRECT_A1D_ELEM(fsc, i));
			if (is_whole_instead_of_half)
			{
				// Factor two because of twice as many particles
				// Sqrt-term to get 60-degree phase errors....
				myfsc = sqrt(2. * myfsc / (myfsc + 1.));
			}
			myfsc = XMIPP_MIN(0.999, myfsc);
			RFLOAT myssnr = myfsc / (1. - myfsc);
			// Sjors 29nov2017 try tau2_fudge for pulling harder on Refine3D runs...
            myssnr *= tau2_fudge;
			RFLOAT fsc_based_tau = myssnr * DIRECT_A1D_ELEM(sigma2, i);
			DIRECT_A1D_ELEM(tau2, i) = fsc_based_tau;
			// data_vs_prior is merely for reporting: it is not used for anything in the reconstruction
			DIRECT_A1D_ELEM(data_vs_prior, i) = myssnr;
		}
		//13

	}
    RCTOCREC(ReconTimer,ReconS_2);
    RCTICREC(ReconTimer,ReconS_2_5);
	// Apply MAP-additional term to the Fnewweight array
	// This will regularise the actual reconstruction
    if (do_map)
	{

    	// Then, add the inverse of tau2-spectrum values to the weight
		// and also calculate spherical average of data_vs_prior ratios
		if (!update_tau2_with_fsc)
		{
			data_vs_prior.initZeros(ori_size/2 + 1);
		}
		fourier_coverage.initZeros(ori_size/2 + 1);
		counter.initZeros(ori_size/2 + 1);
		FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
 		{
			int r2 = kp * kp + ip * ip + jp * jp;
			if (r2 < max_r2)
			{
				int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
				RFLOAT invw = DIRECT_A3D_ELEM(Fweight, k, i, j);

				RFLOAT invtau2;
				if (DIRECT_A1D_ELEM(tau2, ires) > 0.)
				{
					// Calculate inverse of tau2
					invtau2 = 1. / (oversampling_correction * tau2_fudge * DIRECT_A1D_ELEM(tau2, ires));
				}
				else if (DIRECT_A1D_ELEM(tau2, ires) == 0.)
				{
					// If tau2 is zero, use small value instead
					invtau2 = 1./ ( 0.001 * invw);
				}
				else
				{
					std::cerr << " sigma2= " << sigma2 << std::endl;
					std::cerr << " fsc= " << fsc << std::endl;
					std::cerr << " tau2= " << tau2 << std::endl;
					REPORT_ERROR("ERROR BackProjector::reconstruct: Negative or zero values encountered for tau2 spectrum!");
				}

				// Keep track of spectral evidence-to-prior ratio and remaining noise in the reconstruction
				if (!update_tau2_with_fsc)
				{
					DIRECT_A1D_ELEM(data_vs_prior, ires) += invw / invtau2;
				}

				// Keep track of the coverage in Fourier space
				if (invw / invtau2 >= 1.)
					DIRECT_A1D_ELEM(fourier_coverage, ires) += 1.;

				DIRECT_A1D_ELEM(counter, ires) += 1.;

				// Only for (ires >= minres_map) add Wiener-filter like term
				if (ires >= minres_map)
				{
					// Now add the inverse-of-tau2_class term
					invw += invtau2;
					// Store the new weight again in Fweight
					DIRECT_A3D_ELEM(Fweight, k, i, j) = invw;
				}
			}
		}

		// Average data_vs_prior
		if (!update_tau2_with_fsc)
		{
			FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(data_vs_prior)
			{
				if (i > r_max)
					DIRECT_A1D_ELEM(data_vs_prior, i) = 0.;
				else if (DIRECT_A1D_ELEM(counter, i) < 0.001)
					DIRECT_A1D_ELEM(data_vs_prior, i) = 999.;
				else
					DIRECT_A1D_ELEM(data_vs_prior, i) /= DIRECT_A1D_ELEM(counter, i);
			}
		}

		// Calculate Fourier coverage in each shell
		FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(fourier_coverage)
		{
			if (DIRECT_A1D_ELEM(counter, i) > 0.)
				DIRECT_A1D_ELEM(fourier_coverage, i) /= DIRECT_A1D_ELEM(counter, i);
		}

	} //end if do_map
    else if (do_fsc0999)
    {

     	// Sjors 9may2018: avoid numerical instabilities with unregularised reconstructions....
        FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
        {
            int r2 = kp * kp + ip * ip + jp * jp;
            if (r2 < max_r2)
            {
                int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
                if (ires >= minres_map)
                {
                    // add 1/1000th of the radially averaged sigma2 to the Fweight, to avoid having zeros there...
                	DIRECT_A3D_ELEM(Fweight, k, i, j) += 1./(999. * DIRECT_A1D_ELEM(sigma2, ires));
                }
            }
        }

    }


    RCTOCREC(ReconTimer,ReconS_2_5);
	if (skip_gridding)
	{
	    RCTICREC(ReconTimer,ReconS_3);
		std::cerr << "Skipping gridding!" << std::endl;
		Fconv.initZeros(); // to remove any stuff from the input volume
		decenter(data, Fconv, max_r2);

		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			if (DIRECT_MULTIDIM_ELEM(Fweight, n) > 0.)
				DIRECT_MULTIDIM_ELEM(Fconv, n) /= DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		RCTOCREC(ReconTimer,ReconS_3);
#ifdef DEBUG_RECONSTRUCT
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			DIRECT_MULTIDIM_ELEM(ttt(), n) = DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		ttt.write("reconstruct_skipgridding_correction_term.spi");
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			if (DIRECT_MULTIDIM_ELEM(Fweight, n) > 0.)
				DIRECT_MULTIDIM_ELEM(ttt(), n) = 1./DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		ttt.write("reconstruct_skipgridding_correction_term_inverse.spi");
#endif
	}
	else
	{
		RCTICREC(ReconTimer,ReconS_4);
		// Divide both data and Fweight by normalisation factor to prevent FFT's with very large values....
	#ifdef DEBUG_RECONSTRUCT
		std::cerr << " normalise= " << normalise << std::endl;
	#endif
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fweight)
		{
			DIRECT_MULTIDIM_ELEM(Fweight, n) /= normalise;
		}
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(data)
		{
			DIRECT_MULTIDIM_ELEM(data, n) /= normalise;
		}
		RCTOCREC(ReconTimer,ReconS_4);
		RCTICREC(ReconTimer,ReconS_5);
        // Initialise Fnewweight with 1's and 0's. (also see comments below)
		FOR_ALL_ELEMENTS_IN_ARRAY3D(weight)
		{
			if (k * k + i * i + j * j < max_r2)
				A3D_ELEM(weight, k, i, j) = 1.;
			else
				A3D_ELEM(weight, k, i, j) = 0.;
		}

		decenter(weight, Fnewweight, max_r2);
		RCTOCREC(ReconTimer,ReconS_5);

		//for(int z=0;z=)
/*		int z=0;
		for(int y=0;y<Fnewweight.ydim;y++)
		{
			for(int x=0;x<Fnewweight.xdim;x++)
			{
				printf("%f ",Fnewweight.data[z*Fnewweight.xdim*Fnewweight.ydim+y*Fnewweight.xdim+x]);
			}
			printf("\n");
		}

		return ;*/

		// Iterative algorithm as in  Eq. [14] in Pipe & Menon (1999)
		// or Eq. (4) in Matej (2001)
		for (int iter = 0; iter < max_iter_preweight; iter++)
		{
            //std::cout << "    iteration " << (iter+1) << "/" << max_iter_preweight << "\n";
			RCTICREC(ReconTimer,ReconS_6);
			// Set Fnewweight * Fweight in the transformer
			// In Matej et al (2001), weights w_P^i are convoluted with the kernel,
			// and the initial w_P^0 are 1 at each sampling point
			// Here the initial weights are also 1 (see initialisation Fnewweight above),
			// but each "sampling point" counts "Fweight" times!
			// That is why Fnewweight is multiplied by Fweight prior to the convolution


//			printf("data: %ld %ld %ld %ld \n",Fnewweight.ndim,Fnewweight.xdim,Fnewweight.ydim,Fnewweight.zdim);

			FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
			{
				DIRECT_MULTIDIM_ELEM(Fconv, n) = DIRECT_MULTIDIM_ELEM(Fnewweight, n) * DIRECT_MULTIDIM_ELEM(Fweight, n);
			}
/*
			printf("CPU step1: \n");
			for(int i=0;i<0+10;i++)
				printf("%f %f \n",Fconv.data[i].real,Fconv.data[i].imag);
			printf("\n");*/

/*			size_t fullsize= pad_size*pad_size*pad_size;
			cufftComplex *c_Fconv2 = (cufftComplex *)malloc(fullsize * sizeof(cufftComplex));
			layoutchangecomp(Fconv.data,Fconv.xdim,Fconv.ydim,Fconv.zdim,pad_size,c_Fconv2);

			FILE *fp1,*fp2;
			fp1= fopen("/mnt/sdb/wangzihao/Fconvreal.out","w+");
			fp2= fopen("/mnt/sdb/wangzihao/Fconvimag.out","w+");

			double fsum = 0;
			for(int i=0;i<fullsize;i++)
			{
				fprintf(fp1,"%f ",c_Fconv2[i].x);
				fprintf(fp2,"%f ",c_Fconv2[i].y);
				fsum +=c_Fconv2[i].x;
			}
			printf("fsum: %f \n",fsum);
			fclose(fp1);
			fclose(fp2);*/
/*
			fftw_complex *input, *out;
			fullsize=200*200*200;
			input= (fftw_complex *)malloc(fullsize* 2 * sizeof(fftw_complex));
			for(int i=0;i<fullsize;i++)
			{
				input[i][0]=i%100;
				input[i][1]=0;
			}
			fftw_plan p;

			p= fftw_plan_dft_3d(200, 200, 200 ,input, input, FFTW_BACKWARD, FFTW_ESTIMATE);
			fftw_execute(p);
			for(int i=0;i<10;i++)
				printf("%f %f \n",input[i][0],input[i][1]);*/
			//printdatatofile(Fconv.data,Fconv.nzyxdim);
			// convolute through Fourier-transform (as both grids are rectangular)
			// Note that convoluteRealSpace acts on the complex array inside the transformer
			//nr_threads -> iter;
            convoluteBlobRealSpace(transformer, false, iter);


			RFLOAT w, corr_min = LARGE_NUMBER, corr_max = -LARGE_NUMBER, corr_avg=0., corr_nn=0.;

            FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
            {
				if (kp * kp + ip * ip + jp * jp < max_r2)
				{

					// Make sure no division by zero can occur....
					w = XMIPP_MAX(1e-6, abs(DIRECT_A3D_ELEM(Fconv, k, i, j)));
					// Monitor min, max and avg conv_weight
					corr_min = XMIPP_MIN(corr_min, w);
					corr_max = XMIPP_MAX(corr_max, w);
					corr_avg += w;
					corr_nn += 1.;
					// Apply division of Eq. [14] in Pipe & Menon (1999)
					DIRECT_A3D_ELEM(Fnewweight, k, i, j) /= w;
				}
			}
			RCTOCREC(ReconTimer,ReconS_6);


		    //layoutchangecomp(Fnewweight,Fnewweight.xdim,Fnewweight.ydim,Fnewweight.zdim,pad_size,c_Fconv2);
			//printdatatofile(Fnewweight.data,Fnewweight.xdim*Fnewweight.ydim*Fnewweight.zdim,Fnewweight.xdim,1);

	#ifdef DEBUG_RECONSTRUCT
			std::cerr << " PREWEIGHTING ITERATION: "<< iter + 1 << " OF " << max_iter_preweight << std::endl;
			// report of maximum and minimum values of current conv_weight
			std::cerr << " corr_avg= " << corr_avg / corr_nn << std::endl;
			std::cerr << " corr_min= " << corr_min << std::endl;
			std::cerr << " corr_max= " << corr_max << std::endl;
	#endif
		}
/*
		int index=0;
		for(int i=0;i<Fnewweight.nzyxdim;i++)
			if(Fnewweight.data[i]!=0)
			{
				index=i;
				printf("non zero data is %d \n",index);
				break;
			}

		for(int i=index;i<index+10;i++)
			printf("%f ",Fnewweight.data[i]);
		printf("\n");
*/
		RCTICREC(ReconTimer,ReconS_7);
	#ifdef DEBUG_RECONSTRUCT
		Image<double> tttt;
		tttt()=Fnewweight;
		tttt.write("reconstruct_gridding_weight.spi");
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			DIRECT_MULTIDIM_ELEM(ttt(), n) = abs(DIRECT_MULTIDIM_ELEM(Fconv, n));
		}
		ttt.write("reconstruct_gridding_correction_term.spi");
	#endif


		// Clear memory
		Fweight.clear();

		// Note that Fnewweight now holds the approximation of the inverse of the weights on a regular grid

		// Now do the actual reconstruction with the data array
		// Apply the iteratively determined weight
		Fconv.initZeros(); // to remove any stuff from the input volume
		decenter(data, Fconv, max_r2);
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
#ifdef  RELION_SINGLE_PRECISION
			// Prevent numerical instabilities in single-precision reconstruction with very unevenly sampled orientations
			if (DIRECT_MULTIDIM_ELEM(Fnewweight, n) > 1e20)
				DIRECT_MULTIDIM_ELEM(Fnewweight, n) = 1e20;
#endif
			DIRECT_MULTIDIM_ELEM(Fconv, n) *= DIRECT_MULTIDIM_ELEM(Fnewweight, n);
		}

		// Clear memory
		Fnewweight.clear();
		RCTOCREC(ReconTimer,ReconS_7);
	} // end if skip_gridding

// Gridding theory says one now has to interpolate the fine grid onto the coarse one using a blob kernel
// and then do the inverse transform and divide by the FT of the blob (i.e. do the gridding correction)
// In practice, this gives all types of artefacts (perhaps I never found the right implementation?!)
// Therefore, window the Fourier transform and then do the inverse transform
//#define RECONSTRUCT_CONVOLUTE_BLOB
#ifdef RECONSTRUCT_CONVOLUTE_BLOB

	// Apply the same blob-convolution as above to the data array
	// Mask real-space map beyond its original size to prevent aliasing in the downsampling step below
	RCTICREC(ReconTimer,ReconS_8);
	convoluteBlobRealSpace(transformer, true);
	RCTOCREC(ReconTimer,ReconS_8);
	RCTICREC(ReconTimer,ReconS_9);
	// Now just pick every 3rd pixel in Fourier-space (i.e. down-sample)
	// and do a final inverse FT
	if (ref_dim == 2)
		vol_out.resize(ori_size, ori_size);
	else
		vol_out.resize(ori_size, ori_size, ori_size);
	RCTOCREC(ReconTimer,ReconS_9);
	RCTICREC(ReconTimer,ReconS_10);
	FourierTransformer transformer2;
	MultidimArray<Complex > Ftmp;
	transformer2.setReal(vol_out); // cannot use the first transformer because Fconv is inside there!!
	transformer2.getFourierAlias(Ftmp);
	RCTOCREC(ReconTimer,ReconS_10);
	RCTICREC(ReconTimer,ReconS_11);
	FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Ftmp)
	{
		if (kp * kp + ip * ip + jp * jp < r_max * r_max)
		{
			DIRECT_A3D_ELEM(Ftmp, k, i, j) = FFTW_ELEM(Fconv, kp * padding_factor, ip * padding_factor, jp * padding_factor);
		}
		else
		{
			DIRECT_A3D_ELEM(Ftmp, k, i, j) = 0.;
		}
	}
	RCTOCREC(ReconTimer,ReconS_11);
	RCTICREC(ReconTimer,ReconS_12);
	// inverse FFT leaves result in vol_out
	transformer2.inverseFourierTransform();
	RCTOCREC(ReconTimer,ReconS_12);
	RCTICREC(ReconTimer,ReconS_13);
	// Shift the map back to its origin
	CenterFFT(vol_out, false);
	RCTOCREC(ReconTimer,ReconS_13);
	RCTICREC(ReconTimer,ReconS_14);
	// Un-normalize FFTW (because original FFTs were done with the size of 2D FFTs)
	if (ref_dim==3)
		vol_out /= ori_size;
	RCTOCREC(ReconTimer,ReconS_14);
	RCTICREC(ReconTimer,ReconS_15);
	// Mask out corners to prevent aliasing artefacts
	softMaskOutsideMap(vol_out);
	RCTOCREC(ReconTimer,ReconS_15);
	RCTICREC(ReconTimer,ReconS_16);
	// Gridding correction for the blob
	RFLOAT normftblob = tab_ftblob(0.);
	FOR_ALL_ELEMENTS_IN_ARRAY3D(vol_out)
	{

		RFLOAT r = sqrt((RFLOAT)(k*k+i*i+j*j));
		RFLOAT rval = r / (ori_size * padding_factor);
		A3D_ELEM(vol_out, k, i, j) /= tab_ftblob(rval) / normftblob;
		//if (k==0 && i==0)
		//	std::cerr << " j= " << j << " rval= " << rval << " tab_ftblob(rval) / normftblob= " << tab_ftblob(rval) / normftblob << std::endl;
	}
	RCTOCREC(ReconTimer,ReconS_16);

#else

	// rather than doing the blob-convolution to downsample the data array, do a windowing operation:
	// This is the same as convolution with a SINC. It seems to give better maps.
	// Then just make the blob look as much as a SINC as possible....
	// The "standard" r1.9, m2 and a15 blob looks quite like a sinc until the first zero (perhaps that's why it is standard?)
	//for (RFLOAT r = 0.1; r < 10.; r+=0.01)
	//{
	//	RFLOAT sinc = sin(PI * r / padding_factor ) / ( PI * r / padding_factor);
	//	std::cout << " r= " << r << " sinc= " << sinc << " blob= " << blob_val(r, blob) << std::endl;
	//}

	// Now do inverse FFT and window to original size in real-space
	// Pass the transformer to prevent making and clearing a new one before clearing the one declared above....
	// The latter may give memory problems as detected by electric fence....
	RCTICREC(ReconTimer,ReconS_17);
	windowToOridimRealSpace(transformer, vol_out, nr_threads, printTimes);
	RCTOCREC(ReconTimer,ReconS_17);

#endif

#ifdef DEBUG_RECONSTRUCT
	ttt()=vol_out;
	ttt.write("reconstruct_before_gridding_correction.spi");
#endif

	// Correct for the linear/nearest-neighbour interpolation that led to the data array
	RCTICREC(ReconTimer,ReconS_18);
	griddingCorrect(vol_out);
	RCTOCREC(ReconTimer,ReconS_18);
	// If the tau-values were calculated based on the FSC, then now re-calculate the power spectrum of the actual reconstruction
	if (update_tau2_with_fsc)
	{

		// New tau2 will be the power spectrum of the new map
		MultidimArray<RFLOAT> spectrum, count;

		// Calculate this map's power spectrum
		// Don't call getSpectrum() because we want to use the same transformer object to prevent memory trouble....
		RCTICREC(ReconTimer,ReconS_19);
		spectrum.initZeros(XSIZE(vol_out));
	    count.initZeros(XSIZE(vol_out));
		RCTOCREC(ReconTimer,ReconS_19);
		RCTICREC(ReconTimer,ReconS_20);
	    // recycle the same transformer for all images
        transformer.setReal(vol_out);
		RCTOCREC(ReconTimer,ReconS_20);
		RCTICREC(ReconTimer,ReconS_21);
        transformer.FourierTransform();
		RCTOCREC(ReconTimer,ReconS_21);
		RCTICREC(ReconTimer,ReconS_22);
	    FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
	    {
	    	long int idx = ROUND(sqrt(kp*kp + ip*ip + jp*jp));
	    	spectrum(idx) += norm(dAkij(Fconv, k, i, j));
	        count(idx) += 1.;
	    }
	    spectrum /= count;

		// Factor two because of two-dimensionality of the complex plane
		// (just like sigma2_noise estimates, the power spectra should be divided by 2)
		RFLOAT normfft = (ref_dim == 3 && data_dim == 2) ? (RFLOAT)(ori_size * ori_size) : 1.;
		spectrum *= normfft / 2.;

		// New SNR^MAP will be power spectrum divided by the noise in the reconstruction (i.e. sigma2)
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(data_vs_prior)
		{
			DIRECT_MULTIDIM_ELEM(tau2, n) =  tau2_fudge * DIRECT_MULTIDIM_ELEM(spectrum, n);
		}
		RCTOCREC(ReconTimer,ReconS_22);
	}
	RCTICREC(ReconTimer,ReconS_23);
	// Completely empty the transformer object
	transformer.cleanup();
    // Now can use extra mem to move data into smaller array space
    vol_out.shrinkToFit();

	RCTOCREC(ReconTimer,ReconS_23);
#ifdef TIMINGREC
    if(printTimes)
    	ReconTimer.printTimes(true);
#endif


#ifdef DEBUG_RECONSTRUCT
    std::cerr<<"done with reconstruct"<<std::endl;
#endif

	tau2_io = tau2;
    sigma2_out = sigma2;
    data_vs_prior_out = data_vs_prior;
    fourier_coverage_out = fourier_coverage;
}
void BackProjector::reconstruct_onegpu(MultidimArray<RFLOAT> &vol_out,
                                int max_iter_preweight,
                                bool do_map,
                                RFLOAT tau2_fudge,
                                MultidimArray<RFLOAT> &tau2_io, // can be input/output
                                MultidimArray<RFLOAT> &sigma2_out,
                                MultidimArray<RFLOAT> &data_vs_prior_out,
                                MultidimArray<RFLOAT> &fourier_coverage_out,
                                const MultidimArray<RFLOAT> &fsc, // only input
                                RFLOAT normalise,
                                bool update_tau2_with_fsc,
                                bool is_whole_instead_of_half,
                                int nr_threads,
                                int minres_map,
                                bool printTimes,
								bool do_fsc0999)
{


}
void BackProjector::reconstruct_gpu_raw(MultidimArray<RFLOAT> &vol_out,
                                int max_iter_preweight,
                                bool do_map,
                                RFLOAT tau2_fudge,
                                MultidimArray<RFLOAT> &tau2_io, // can be input/output
                                MultidimArray<RFLOAT> &sigma2_out,
                                MultidimArray<RFLOAT> &data_vs_prior_out,
                                MultidimArray<RFLOAT> &fourier_coverage_out,
                                const MultidimArray<RFLOAT> &fsc, // only input
                                RFLOAT normalise,
                                bool update_tau2_with_fsc,
                                bool is_whole_instead_of_half,
                                int nr_threads,
                                int minres_map,
                                bool printTimes,
								bool do_fsc0999)
{

#ifdef TIMINGREC
	Timer ReconTimer;
	printTimes=1;
	int ReconS_1 = ReconTimer.setNew(" RcS1_Init ");
	int ReconS_2 = ReconTimer.setNew(" RcS2_Shape&Noise ");
	int ReconS_2_5 = ReconTimer.setNew(" RcS2.5_Regularize ");
	int ReconS_3 = ReconTimer.setNew(" RcS3_skipGridding ");
	int ReconS_4 = ReconTimer.setNew(" RcS4_doGridding_norm ");
	int ReconS_5 = ReconTimer.setNew(" RcS5_doGridding_init ");
	int ReconS_6 = ReconTimer.setNew(" RcS6_doGridding_iter ");
	int ReconS_7 = ReconTimer.setNew(" RcS7_doGridding_apply ");
	int ReconS_8 = ReconTimer.setNew(" RcS8_blobConvolute ");
	int ReconS_9 = ReconTimer.setNew(" RcS9_blobResize ");
	int ReconS_10 = ReconTimer.setNew(" RcS10_blobSetReal ");
	int ReconS_11 = ReconTimer.setNew(" RcS11_blobSetTemp ");
	int ReconS_12 = ReconTimer.setNew(" RcS12_blobTransform ");
	int ReconS_13 = ReconTimer.setNew(" RcS13_blobCenterFFT ");
	int ReconS_14 = ReconTimer.setNew(" RcS14_blobNorm1 ");
	int ReconS_15 = ReconTimer.setNew(" RcS15_blobSoftMask ");
	int ReconS_16 = ReconTimer.setNew(" RcS16_blobNorm2 ");
	int ReconS_17 = ReconTimer.setNew(" RcS17_WindowReal ");
	int ReconS_18 = ReconTimer.setNew(" RcS18_GriddingCorrect ");
	int ReconS_19 = ReconTimer.setNew(" RcS19_tauInit ");
	int ReconS_20 = ReconTimer.setNew(" RcS20_tausetReal ");
	int ReconS_21 = ReconTimer.setNew(" RcS21_tauTransform ");
	int ReconS_22 = ReconTimer.setNew(" RcS22_tautauRest ");
	int ReconS_23 = ReconTimer.setNew(" RcS23_tauShrinkToFit ");
	int ReconS_24 = ReconTimer.setNew(" RcS24_extra ");
#endif

    // never rely on references (handed to you from the outside) for computation:
    // they could be the same (i.e. reconstruct(..., dummy, dummy, dummy, dummy, ...); )

    MultidimArray<RFLOAT> sigma2, data_vs_prior, fourier_coverage;
	MultidimArray<RFLOAT> tau2 = tau2_io;


    RCTICREC(ReconTimer,ReconS_1);
    FourierTransformer transformer;
	MultidimArray<RFLOAT> Fweight;
	// Fnewweight can become too large for a float: always keep this one in double-precision
	MultidimArray<double> Fnewweight;
	MultidimArray<Complex>& Fconv = transformer.getFourierReference();
	int max_r2 = ROUND(r_max * padding_factor) * ROUND(r_max * padding_factor);

//#define DEBUG_RECONSTRUCT
#ifdef DEBUG_RECONSTRUCT
	Image<RFLOAT> ttt;
	FileName fnttt;
	ttt()=weight;
	ttt.write("reconstruct_initial_weight.spi");
	std::cerr << " pad_size= " << pad_size << " padding_factor= " << padding_factor << " max_r2= " << max_r2 << std::endl;
#endif

    // Set Fweight, Fnewweight and Fconv to the right size
    if (ref_dim == 2)
        vol_out.setDimensions(pad_size, pad_size, 1, 1);
    else
        // Too costly to actually allocate the space
        // Trick transformer with the right dimensions
        vol_out.setDimensions(pad_size, pad_size, pad_size, 1);

    transformer.setReal(vol_out); // Fake set real. 1. Allocate space for Fconv 2. calculate plans.
    vol_out.clear(); // Reset dimensions to 0

    RCTOCREC(ReconTimer,ReconS_1);
    RCTICREC(ReconTimer,ReconS_2);

    Fweight.reshape(Fconv);
    if (!skip_gridding)
    	Fnewweight.reshape(Fconv);

	// Go from projector-centered to FFTW-uncentered
	decenter(weight, Fweight, max_r2);

	// Take oversampling into account
	RFLOAT oversampling_correction = (ref_dim == 3) ? (padding_factor * padding_factor * padding_factor) : (padding_factor * padding_factor);
	MultidimArray<RFLOAT> counter;

	// First calculate the radial average of the (inverse of the) power of the noise in the reconstruction
	// This is the left-hand side term in the nominator of the Wiener-filter-like update formula
	// and it is stored inside the weight vector
	// Then, if (do_map) add the inverse of tau2-spectrum values to the weight
	sigma2.initZeros(ori_size/2 + 1);
	counter.initZeros(ori_size/2 + 1);
	FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
	{
		int r2 = kp * kp + ip * ip + jp * jp;
		if (r2 < max_r2)
		{
			int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
			RFLOAT invw = oversampling_correction * DIRECT_A3D_ELEM(Fweight, k, i, j);
			DIRECT_A1D_ELEM(sigma2, ires) += invw;
			DIRECT_A1D_ELEM(counter, ires) += 1.;
		}
    }

	// Average (inverse of) sigma2 in reconstruction
	FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(sigma2)
	{
        if (DIRECT_A1D_ELEM(sigma2, i) > 1e-10)
            DIRECT_A1D_ELEM(sigma2, i) = DIRECT_A1D_ELEM(counter, i) / DIRECT_A1D_ELEM(sigma2, i);
        else if (DIRECT_A1D_ELEM(sigma2, i) == 0)
            DIRECT_A1D_ELEM(sigma2, i) = 0.;
		else
		{
			std::cerr << " DIRECT_A1D_ELEM(sigma2, i)= " << DIRECT_A1D_ELEM(sigma2, i) << std::endl;
			REPORT_ERROR("BackProjector::reconstruct: ERROR: unexpectedly small, yet non-zero sigma2 value, this should not happen...a");
        }
    }

	if (update_tau2_with_fsc)
    {
        tau2.reshape(ori_size/2 + 1);
        data_vs_prior.initZeros(ori_size/2 + 1);
		// Then calculate new tau2 values, based on the FSC
		if (!fsc.sameShape(sigma2) || !fsc.sameShape(tau2))
		{
			fsc.printShape(std::cerr);
			tau2.printShape(std::cerr);
			sigma2.printShape(std::cerr);
			REPORT_ERROR("ERROR BackProjector::reconstruct: sigma2, tau2 and fsc have different sizes");
		}
		FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(sigma2)
        {
			// FSC cannot be negative or zero for conversion into tau2
			RFLOAT myfsc = XMIPP_MAX(0.001, DIRECT_A1D_ELEM(fsc, i));
			if (is_whole_instead_of_half)
			{
				// Factor two because of twice as many particles
				// Sqrt-term to get 60-degree phase errors....
				myfsc = sqrt(2. * myfsc / (myfsc + 1.));
			}
			myfsc = XMIPP_MIN(0.999, myfsc);
			RFLOAT myssnr = myfsc / (1. - myfsc);
			// Sjors 29nov2017 try tau2_fudge for pulling harder on Refine3D runs...
            myssnr *= tau2_fudge;
			RFLOAT fsc_based_tau = myssnr * DIRECT_A1D_ELEM(sigma2, i);
			DIRECT_A1D_ELEM(tau2, i) = fsc_based_tau;
			// data_vs_prior is merely for reporting: it is not used for anything in the reconstruction
			DIRECT_A1D_ELEM(data_vs_prior, i) = myssnr;
		}
	}
    RCTOCREC(ReconTimer,ReconS_2);
    RCTICREC(ReconTimer,ReconS_2_5);
	// Apply MAP-additional term to the Fnewweight array
	// This will regularise the actual reconstruction
    if (do_map)
	{

    	// Then, add the inverse of tau2-spectrum values to the weight
		// and also calculate spherical average of data_vs_prior ratios
		if (!update_tau2_with_fsc)
			data_vs_prior.initZeros(ori_size/2 + 1);
		fourier_coverage.initZeros(ori_size/2 + 1);
		counter.initZeros(ori_size/2 + 1);
		FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
 		{
			int r2 = kp * kp + ip * ip + jp * jp;
			if (r2 < max_r2)
			{
				int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
				RFLOAT invw = DIRECT_A3D_ELEM(Fweight, k, i, j);

				RFLOAT invtau2;
				if (DIRECT_A1D_ELEM(tau2, ires) > 0.)
				{
					// Calculate inverse of tau2
					invtau2 = 1. / (oversampling_correction * tau2_fudge * DIRECT_A1D_ELEM(tau2, ires));
				}
				else if (DIRECT_A1D_ELEM(tau2, ires) == 0.)
				{
					// If tau2 is zero, use small value instead
					invtau2 = 1./ ( 0.001 * invw);
				}
				else
				{
					std::cerr << " sigma2= " << sigma2 << std::endl;
					std::cerr << " fsc= " << fsc << std::endl;
					std::cerr << " tau2= " << tau2 << std::endl;
					REPORT_ERROR("ERROR BackProjector::reconstruct: Negative or zero values encountered for tau2 spectrum!");
				}

				// Keep track of spectral evidence-to-prior ratio and remaining noise in the reconstruction
				if (!update_tau2_with_fsc)
					DIRECT_A1D_ELEM(data_vs_prior, ires) += invw / invtau2;

				// Keep track of the coverage in Fourier space
				if (invw / invtau2 >= 1.)
					DIRECT_A1D_ELEM(fourier_coverage, ires) += 1.;

				DIRECT_A1D_ELEM(counter, ires) += 1.;

				// Only for (ires >= minres_map) add Wiener-filter like term
				if (ires >= minres_map)
				{
					// Now add the inverse-of-tau2_class term
					invw += invtau2;
					// Store the new weight again in Fweight
					DIRECT_A3D_ELEM(Fweight, k, i, j) = invw;
				}
			}
		}

		// Average data_vs_prior
		if (!update_tau2_with_fsc)
		{
			FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(data_vs_prior)
			{
				if (i > r_max)
					DIRECT_A1D_ELEM(data_vs_prior, i) = 0.;
				else if (DIRECT_A1D_ELEM(counter, i) < 0.001)
					DIRECT_A1D_ELEM(data_vs_prior, i) = 999.;
				else
					DIRECT_A1D_ELEM(data_vs_prior, i) /= DIRECT_A1D_ELEM(counter, i);
			}
		}

		// Calculate Fourier coverage in each shell
		FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(fourier_coverage)
		{
			if (DIRECT_A1D_ELEM(counter, i) > 0.)
				DIRECT_A1D_ELEM(fourier_coverage, i) /= DIRECT_A1D_ELEM(counter, i);
		}

	} //end if do_map
    else if (do_fsc0999)
    {

     	// Sjors 9may2018: avoid numerical instabilities with unregularised reconstructions....
        FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
        {
            int r2 = kp * kp + ip * ip + jp * jp;
            if (r2 < max_r2)
            {
                int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
                if (ires >= minres_map)
                {
                    // add 1/1000th of the radially averaged sigma2 to the Fweight, to avoid having zeros there...
                	DIRECT_A3D_ELEM(Fweight, k, i, j) += 1./(999. * DIRECT_A1D_ELEM(sigma2, ires));
                }
            }
        }

    }

//==============================================================================add  GPU version

	int Ndim[3];
	int GPU_N=2;
	Ndim[0]=pad_size;
	Ndim[1]=pad_size;
	Ndim[2]=pad_size;
	size_t fullsize= pad_size*pad_size*pad_size;
    initgpu(GPU_N);
	RFLOAT *d_Fweight;
	double *d_Fnewweight;
	int offset;

	MultiGPUplan *dataplan;
	dataplan = (MultiGPUplan *)malloc(sizeof(MultiGPUplan)*2);
	multi_plan_init(dataplan,GPU_N,fullsize,Ndim[0],Ndim[1],Ndim[2]);
	cufftComplex *c_Fconv,*c_Fconv2;

	for (int i = 0; i < GPU_N; ++i) {
		cudaSetDevice(dataplan[i].devicenum);
		cudaMalloc((void**) &(dataplan[i].d_Data),sizeof(cufftComplex) * dataplan[i].datasize);
	}

	//=================================
	int xyN[2];
	xyN[0] = Ndim[0];
	xyN[1] = Ndim[1];
	cufftHandle xyplan[2];
	cufftHandle xyextra;
	cufftHandle zplan[2];
	cufftHandle zplanextra;
	int fftoffset[2];
	fftoffset[0] = 0;
	fftoffset[1] = Ndim[0] * (Ndim[1] / 2);
	//===================================


    RCTOCREC(ReconTimer,ReconS_2_5);
	if (skip_gridding)
	{
	    RCTICREC(ReconTimer,ReconS_3);
		std::cerr << "Skipping gridding!" << std::endl;
		Fconv.initZeros(); // to remove any stuff from the input volume
		decenter(data, Fconv, max_r2);

		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			if (DIRECT_MULTIDIM_ELEM(Fweight, n) > 0.)
				DIRECT_MULTIDIM_ELEM(Fconv, n) /= DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		RCTOCREC(ReconTimer,ReconS_3);
#ifdef DEBUG_RECONSTRUCT
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			DIRECT_MULTIDIM_ELEM(ttt(), n) = DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		ttt.write("reconstruct_skipgridding_correction_term.spi");
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			if (DIRECT_MULTIDIM_ELEM(Fweight, n) > 0.)
				DIRECT_MULTIDIM_ELEM(ttt(), n) = 1./DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		ttt.write("reconstruct_skipgridding_correction_term_inverse.spi");
#endif
	}
	else
	{
		RCTICREC(ReconTimer,ReconS_4);
		// Divide both data and Fweight by normalisation factor to prevent FFT's with very large values....
	#ifdef DEBUG_RECONSTRUCT
		std::cerr << " normalise= " << normalise << std::endl;
	#endif
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fweight)
		{
			DIRECT_MULTIDIM_ELEM(Fweight, n) /= normalise;
		}
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(data)
		{
			DIRECT_MULTIDIM_ELEM(data, n) /= normalise;
		}
		RCTOCREC(ReconTimer,ReconS_4);
		RCTICREC(ReconTimer,ReconS_5);
        // Initialise Fnewweight with 1's and 0's. (also see comments below)
		FOR_ALL_ELEMENTS_IN_ARRAY3D(weight)
		{
			if (k * k + i * i + j * j < max_r2)
				A3D_ELEM(weight, k, i, j) = 1.;
			else
				A3D_ELEM(weight, k, i, j) = 0.;
		}



		decenter(weight, Fnewweight, max_r2);
		RCTOCREC(ReconTimer,ReconS_5);
		// Iterative algorithm as in  Eq. [14] in Pipe & Menon (1999)
		// or Eq. (4) in Matej (2001)



		long int Fconvnum=Fconv.nzyxdim;


		long int normsize= Ndim[0]*Ndim[1]*Ndim[2];
		int halfxdim=Fconv.xdim;

		printf("%ld %ld %ld \n",Fconv.xdim,Fconv.ydim,Fconv.zdim);
		printf("Fnewweight: %ld %ld %ld \n",Fnewweight.xdim,Fnewweight.ydim,Fnewweight.zdim);

		int NZ= Ndim[2];
//======for whole divide num along Z ============================================
		int offsetZ[4];
		int dividnum=2;
		int i;
		for (i = 0; i < dividnum; i++)
			offsetZ[i] = NZ / dividnum;
		offsetZ[i - 1] = NZ - (NZ / dividnum) * (dividnum-1);
		int extraz = offsetZ[i-1] - offsetZ[i-2];
		printf("offset :%d %d extraz: %d \n", offsetZ[0], offsetZ[1], extraz);
//=============================
		dataplan[0].selfZ = offsetZ[0]; //+ offsetZ[1];
		dataplan[1].selfZ = offsetZ[1]; //+ offsetZ[3];

		dataplan[0].realsize=Ndim[0]*Ndim[1]*dataplan[0].selfZ;
		dataplan[1].realsize=Ndim[0]*Ndim[1]*dataplan[1].selfZ;

		multi_enable_access(dataplan,GPU_N);

		c_Fconv = (cufftComplex *)malloc(fullsize * sizeof(cufftComplex));
		//c_Fconv2 = (cufftComplex *)malloc(fullsize * sizeof(cufftComplex));
		cudaMallocHost((void **) &c_Fconv2, sizeof(cufftComplex) * fullsize);

#ifdef RELION_SINGLE_PRECISION
		d_Fweight = gpusetdata_float(d_Fweight,Fweight.nzyxdim,Fweight.data);
#else
		d_Fweight = gpusetdata_double(d_Fweight,Fweight.nzyxdim,Fweight.data);
#endif
		d_Fnewweight = gpusetdata_double(d_Fnewweight,Fnewweight.nzyxdim,Fnewweight.data);


         //==================2d fft plan

		for (int i = 0; i < GPU_N; i++) {
		    cudaSetDevice(dataplan[i].devicenum);
			cufftPlanMany(&xyplan[i], 2, xyN, NULL, 0, 0, NULL, 0, 0, CUFFT_C2C, NZ / 2);
			if (i == 1)
				cufftPlanMany(&xyextra, 2, xyN, NULL, 0, 0, NULL, 0, 0, CUFFT_C2C, extraz);
		}
	    //==================1d fft plan
		int Ncol[1];
		Ncol[0] = Ndim[1];
		int inembed[3], extraembed[3];
		inembed[0] = Ndim[0];inembed[1] = offsetZ[0];inembed[2] = Ndim[2];
		extraembed[0] = Ndim[0];extraembed[1] = offsetZ[1];extraembed[2] = Ndim[2];
		cudaSetDevice(0);
		cufftPlanMany(&zplan[0], 1, Ncol, inembed, Ndim[0] * Ndim[1], 1,inembed, Ndim[0] * Ndim[1], 1, CUFFT_C2C, Ndim[0] * (offsetZ[0]));
		cudaSetDevice(1);
		cufftPlanMany(&zplan[1], 1, Ncol, extraembed, Ndim[0] * Ndim[1], 1, extraembed, Ndim[0] * Ndim[1], 1, CUFFT_C2C, Ndim[0] * (offsetZ[1]));

		struct timeval tv1,tv2;
		struct timezone tz;

		float time_use=0;


		for (int iter = 0; iter < max_iter_preweight; iter++)
		{

            //std::cout << "    iteration " << (iter+1) << "/" << max_iter_preweight << "\n";
			RCTICREC(ReconTimer,ReconS_6);

			vector_Multi_layout(d_Fnewweight,d_Fweight,dataplan[0].d_Data,fullsize,Fconv.xdim,pad_size);
			gettimeofday (&tv1, &tz);
			cudaMemcpy(c_Fconv,dataplan[0].d_Data,fullsize*sizeof(cufftComplex),cudaMemcpyDeviceToHost);

			multi_memcpy_data_gpu(dataplan,GPU_N,Ndim[0],Ndim[1]);
			gettimeofday (&tv2, &tz);
			time_use +=1000 * (tv2.tv_sec-tv1.tv_sec)+ (tv2.tv_usec-tv1.tv_usec)/1000;
			offset = 0;
			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(dataplan[i].devicenum);
				cufftExecC2C(xyplan[i], dataplan[i].d_Data + offset,dataplan[i].d_Data + offset, CUFFT_INVERSE);
				offset += Ndim[0] * Ndim[1] * (Ndim[2] / 2);
			}
			//extra FFT
			cufftExecC2C(xyextra, dataplan[1].d_Data + (Ndim[2] - extraz) * Ndim[0] * Ndim[1],
					dataplan[1].d_Data + (Ndim[2] - extraz) * Ndim[0] * Ndim[1], CUFFT_INVERSE);



			gettimeofday (&tv1, &tz);
			multi_sync(dataplan,GPU_N);
			mulit_alltoall_one(dataplan,Ndim[0],Ndim[1],Ndim[2],extraz,offsetZ);
			multi_sync(dataplan,GPU_N);
			gettimeofday (&tv2, &tz);
			time_use +=1000 * (tv2.tv_sec-tv1.tv_sec)+ (tv2.tv_usec-tv1.tv_usec)/1000;

			cudaSetDevice(0);
			cufftExecC2C(zplan[0], dataplan[0].d_Data + fftoffset[0],dataplan[0].d_Data + fftoffset[0], CUFFT_INVERSE);
			cudaSetDevice(1);
			cufftExecC2C(zplan[1], dataplan[1].d_Data + fftoffset[1],dataplan[1].d_Data + fftoffset[1], CUFFT_INVERSE);
			multi_sync(dataplan,GPU_N);

			gettimeofday (&tv1, &tz);
			mulit_alltoall_all1to0(dataplan,Ndim[0],Ndim[1],Ndim[2],extraz,offsetZ);
			gettimeofday (&tv2, &tz);
			time_use +=1000 * (tv2.tv_sec-tv1.tv_sec)+ (tv2.tv_usec-tv1.tv_usec)/1000;
			cudaDeviceSynchronize();




			RFLOAT normftblob = tab_ftblob(0.);
			RFLOAT *d_tab_ftblob;
			int tabxdim=tab_ftblob.tabulatedValues.xdim;
#ifdef RELION_SINGLE_PRECISION
			d_tab_ftblob=gpusetdata_float(d_tab_ftblob,tab_ftblob.tabulatedValues.xdim,tab_ftblob.tabulatedValues.data);
#else
			d_tab_ftblob=gpusetdata_double(d_tab_ftblob,tab_ftblob.tabulatedValues.xdim,tab_ftblob.tabulatedValues.data);
#endif

			//gpu_kernel2
			volume_Multi_float(dataplan[0].d_Data,d_tab_ftblob, Ndim[0]*Ndim[1]*Ndim[2], tabxdim, tab_ftblob.sampling , pad_size/2, pad_size, ori_size, padding_factor, normftblob);
			cudaDeviceSynchronize();


			gettimeofday (&tv1, &tz);
			mulit_datacopy_0to1(dataplan, Ndim[0],Ndim[1],offsetZ);
			gettimeofday (&tv2, &tz);
			time_use +=1000 * (tv2.tv_sec-tv1.tv_sec)+ (tv2.tv_usec-tv1.tv_usec)/1000;
//=========================EXE CUFFT_FORWARD
			offset =0;
			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(dataplan[i].devicenum);
				cufftExecC2C(xyplan[i], dataplan[i].d_Data + offset,dataplan[i].d_Data + offset, CUFFT_FORWARD);
				offset += Ndim[0] * Ndim[1] * (Ndim[2] / 2);

			}
			//extra FFT
			cufftExecC2C(xyextra, dataplan[1].d_Data + (Ndim[2] - extraz) * Ndim[0] * Ndim[1],
					dataplan[1].d_Data + (Ndim[2] - extraz) * Ndim[0] * Ndim[1], CUFFT_FORWARD);
			multi_sync(dataplan,GPU_N);
			gettimeofday (&tv1, &tz);
			mulit_alltoall_one(dataplan,Ndim[0],Ndim[1],Ndim[2],extraz,offsetZ);
			multi_sync(dataplan,GPU_N);
			gettimeofday (&tv2, &tz);
			time_use +=1000 * (tv2.tv_sec-tv1.tv_sec)+ (tv2.tv_usec-tv1.tv_usec)/1000;
			cudaSetDevice(0);
			cufftExecC2C(zplan[0], dataplan[0].d_Data + fftoffset[0],dataplan[0].d_Data + fftoffset[0], CUFFT_FORWARD);
			cudaSetDevice(1);
			cufftExecC2C(zplan[1], dataplan[1].d_Data + fftoffset[1],
					dataplan[1].d_Data + fftoffset[1], CUFFT_FORWARD);
			multi_sync(dataplan,GPU_N);
			gettimeofday (&tv1, &tz);
			mulit_alltoall_all1to0(dataplan,Ndim[0],Ndim[1],Ndim[2],extraz,offsetZ);
			multi_sync(dataplan,GPU_N);
			gettimeofday (&tv2, &tz);
			time_use +=1000 * (tv2.tv_sec-tv1.tv_sec)+ (tv2.tv_usec-tv1.tv_usec)/1000;
			vector_Normlize(dataplan[0].d_Data, normsize ,Ndim[0]*Ndim[1]*Ndim[2]);

			//gpu_kernel3



			fft_Divide(dataplan[0].d_Data,d_Fnewweight,fullsize,pad_size*pad_size,pad_size,pad_size,pad_size,halfxdim,max_r2);

			RCTOCREC(ReconTimer,ReconS_6);
			//cudaMemcpy(c_Fconv,dataplan[0].d_Data,100*sizeof(cufftComplex),cudaMemcpyDeviceToHost);
			//printf("step iter end  from gpu : \n");

	#ifdef DEBUG_RECONSTRUCT
			std::cerr << " PREWEIGHTING ITERATION: "<< iter + 1 << " OF " << max_iter_preweight << std::endl;
			// report of maximum and minimum values of current conv_weight
			std::cerr << " corr_avg= " << corr_avg / corr_nn << std::endl;
			std::cerr << " corr_min= " << corr_min << std::endl;
			std::cerr << " corr_max= " << corr_max << std::endl;
	#endif

		}

		printf("\ncpy time : %f \n",time_use);

		cudaMemcpy(Fnewweight.data,d_Fnewweight,Fnewweight.nzyxdim*sizeof(double),cudaMemcpyDeviceToHost);

/*		double sum=0;
		printf("TEMP res :\n");
		for(int i=0;i<Fnewweight.xdim*Fnewweight.ydim*Fnewweight.zdim;i++)
		{
			if(i<10)
				printf("%f ",Fnewweight.data[i]);
			sum+=Fnewweight.data[i];
		}
		printf("sum : %f \n",sum);*/



		for (int i = 0; i < GPU_N; i++) {
			cudaSetDevice(dataplan[i].devicenum);
			cufftDestroy(xyplan[i]);
			cufftDestroy(zplan[i]);
			if (i == 1)
				cufftDestroy(xyextra);
		}
		cudaFree(d_Fnewweight);
		cudaFree(d_Fweight);

		RCTICREC(ReconTimer,ReconS_7);

	#ifdef DEBUG_RECONSTRUCT
		Image<double> tttt;
		tttt()=Fnewweight;
		tttt.write("reconstruct_gridding_weight.spi");
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			DIRECT_MULTIDIM_ELEM(ttt(), n) = abs(DIRECT_MULTIDIM_ELEM(Fconv, n));
		}
		ttt.write("reconstruct_gridding_correction_term.spi");
	#endif


		// Clear memory
		Fweight.clear();

		// Note that Fnewweight now holds the approximation of the inverse of the weights on a regular grid

		// Now do the actual reconstruction with the data array
		// Apply the iteratively determined weight
		Fconv.initZeros(); // to remove any stuff from the input volume
		decenter(data, Fconv, max_r2);
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
#ifdef  RELION_SINGLE_PRECISION
			// Prevent numerical instabilities in single-precision reconstruction with very unevenly sampled orientations
			if (DIRECT_MULTIDIM_ELEM(Fnewweight, n) > 1e20)
				DIRECT_MULTIDIM_ELEM(Fnewweight, n) = 1e20;
#endif
			DIRECT_MULTIDIM_ELEM(Fconv, n) *= DIRECT_MULTIDIM_ELEM(Fnewweight, n);
		}

		// Clear memory
		Fnewweight.clear();
		RCTOCREC(ReconTimer,ReconS_7);
	} // end if skip_gridding

// Gridding theory says one now has to interpolate the fine grid onto the coarse one using a blob kernel
// and then do the inverse transform and divide by the FT of the blob (i.e. do the gridding correction)
// In practice, this gives all types of artefacts (perhaps I never found the right implementation?!)
// Therefore, window the Fourier transform and then do the inverse transform
//#define RECONSTRUCT_CONVOLUTE_BLOB
#ifdef RECONSTRUCT_CONVOLUTE_BLOB

	// Apply the same blob-convolution as above to the data array
	// Mask real-space map beyond its original size to prevent aliasing in the downsampling step below
	RCTICREC(ReconTimer,ReconS_8);
	convoluteBlobRealSpace(transformer, true);
	RCTOCREC(ReconTimer,ReconS_8);
	RCTICREC(ReconTimer,ReconS_9);
	// Now just pick every 3rd pixel in Fourier-space (i.e. down-sample)
	// and do a final inverse FT
	if (ref_dim == 2)
		vol_out.resize(ori_size, ori_size);
	else
		vol_out.resize(ori_size, ori_size, ori_size);
	RCTOCREC(ReconTimer,ReconS_9);
	RCTICREC(ReconTimer,ReconS_10);
	FourierTransformer transformer2;
	MultidimArray<Complex > Ftmp;
	transformer2.setReal(vol_out); // cannot use the first transformer because Fconv is inside there!!
	transformer2.getFourierAlias(Ftmp);
	RCTOCREC(ReconTimer,ReconS_10);
	RCTICREC(ReconTimer,ReconS_11);
	FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Ftmp)
	{
		if (kp * kp + ip * ip + jp * jp < r_max * r_max)
		{
			DIRECT_A3D_ELEM(Ftmp, k, i, j) = FFTW_ELEM(Fconv, kp * padding_factor, ip * padding_factor, jp * padding_factor);
		}
		else
		{
			DIRECT_A3D_ELEM(Ftmp, k, i, j) = 0.;
		}
	}
	RCTOCREC(ReconTimer,ReconS_11);
	RCTICREC(ReconTimer,ReconS_12);
	// inverse FFT leaves result in vol_out
	transformer2.inverseFourierTransform();
	RCTOCREC(ReconTimer,ReconS_12);
	RCTICREC(ReconTimer,ReconS_13);
	// Shift the map back to its origin
	CenterFFT(vol_out, false);
	RCTOCREC(ReconTimer,ReconS_13);
	RCTICREC(ReconTimer,ReconS_14);
	// Un-normalize FFTW (because original FFTs were done with the size of 2D FFTs)
	if (ref_dim==3)
		vol_out /= ori_size;
	RCTOCREC(ReconTimer,ReconS_14);
	RCTICREC(ReconTimer,ReconS_15);
	// Mask out corners to prevent aliasing artefacts
	softMaskOutsideMap(vol_out);
	RCTOCREC(ReconTimer,ReconS_15);
	RCTICREC(ReconTimer,ReconS_16);
	// Gridding correction for the blob
	RFLOAT normftblob = tab_ftblob(0.);
	FOR_ALL_ELEMENTS_IN_ARRAY3D(vol_out)
	{

		RFLOAT r = sqrt((RFLOAT)(k*k+i*i+j*j));
		RFLOAT rval = r / (ori_size * padding_factor);
		A3D_ELEM(vol_out, k, i, j) /= tab_ftblob(rval) / normftblob;
		//if (k==0 && i==0)
		//	std::cerr << " j= " << j << " rval= " << rval << " tab_ftblob(rval) / normftblob= " << tab_ftblob(rval) / normftblob << std::endl;
	}
	RCTOCREC(ReconTimer,ReconS_16);

#else

	// rather than doing the blob-convolution to downsample the data array, do a windowing operation:
	// This is the same as convolution with a SINC. It seems to give better maps.
	// Then just make the blob look as much as a SINC as possible....
	// The "standard" r1.9, m2 and a15 blob looks quite like a sinc until the first zero (perhaps that's why it is standard?)
	//for (RFLOAT r = 0.1; r < 10.; r+=0.01)
	//{
	//	RFLOAT sinc = sin(PI * r / padding_factor ) / ( PI * r / padding_factor);
	//	std::cout << " r= " << r << " sinc= " << sinc << " blob= " << blob_val(r, blob) << std::endl;
	//}

	// Now do inverse FFT and window to original size in real-space
	// Pass the transformer to prevent making and clearing a new one before clearing the one declared above....
	// The latter may give memory problems as detected by electric fence....
	RCTICREC(ReconTimer,ReconS_17);

	// Size of padded real-space volume
	int padoridim = ROUND(padding_factor * ori_size);
	// make sure padoridim is even
	padoridim += padoridim%2;
    vol_out.reshape(padoridim, padoridim, padoridim);
    vol_out.setXmippOrigin();
	fullsize = padoridim *padoridim*padoridim;
	multi_plan_init(dataplan,GPU_N,fullsize,padoridim,padoridim,padoridim);
	if(padoridim > pad_size)
	{
		for (int i = 0; i < GPU_N; ++i) {
			cudaSetDevice(dataplan[i].devicenum);
			cudaFree((dataplan[i].d_Data));
			cudaMalloc((void**) &(dataplan[i].d_Data),sizeof(cufftComplex) * dataplan[i].datasize);
		}
	    cudaFreeHost(c_Fconv2);
	    cudaMallocHost((void **) &c_Fconv2, sizeof(cufftComplex) * fullsize);

	}

	windowFourierTransform(Fconv, padoridim);

//	printf("layoutchangecomp : %ld %ld %ld %d\n",Fconv.xdim,Fconv.ydim,Fconv.zdim,padoridim);
	layoutchangecomp(Fconv.data,Fconv.xdim,Fconv.ydim,Fconv.zdim,padoridim,c_Fconv2);
	printwhole(c_Fconv2, padoridim*padoridim*padoridim,0);
//	printf("layoutchangecomp : %ld %ld %ld %d\n",Fconv.xdim,Fconv.ydim,Fconv.zdim,padoridim);
//init plan and for even data



	dataplan[0].selfZ=padoridim/2;
	dataplan[1].selfZ=padoridim/2;
	int offsetz[2];
	offsetz[0]= dataplan[0].selfZ;
	offsetz[1]= dataplan[1].selfZ;
	fftoffset[0] = 0;
	fftoffset[1] = padoridim * (padoridim / 2);
	xyN[0] = padoridim;
	xyN[1] = padoridim;
    //==================2d fft plan

	for (int i = 0; i < GPU_N; i++) {
	    cudaSetDevice(dataplan[i].devicenum);
		cufftPlanMany(&xyplan[i], 2, xyN, NULL, 0, 0, NULL, 0, 0, CUFFT_C2C, padoridim / 2);
	}
   //==================1d fft plan
	int Ncol[1];
	Ncol[0] = padoridim;
	int inembed[3];
	inembed[0] = padoridim;inembed[1] = dataplan[0].selfZ;inembed[2] = padoridim;
	for (int i = 0; i < GPU_N; i++) {
	    cudaSetDevice(dataplan[i].devicenum);
	    cufftPlanMany(&zplan[i], 1, Ncol, inembed, padoridim * padoridim, 1,inembed, padoridim * padoridim, 1, CUFFT_C2C, padoridim * (dataplan[i].selfZ));
	}

	multi_memcpy_data(dataplan,c_Fconv2,GPU_N,padoridim,padoridim);
	offset = 0;
	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(dataplan[i].devicenum);
		cufftExecC2C(xyplan[i], dataplan[i].d_Data + offset,dataplan[i].d_Data + offset, CUFFT_INVERSE);
		offset += padoridim * padoridim * (padoridim / 2);
	}
	multi_sync(dataplan,GPU_N);

	for (int ig = 0; ig < GPU_N; ig++) {

		cudaSetDevice(dataplan[ig].devicenum);
		cudaMemcpy(c_Fconv2,dataplan[ig].d_Data,padoridim*padoridim*padoridim*sizeof(cufftComplex),cudaMemcpyDeviceToHost);
		int starindex=(0);
		int endindex=(fullsize/2);
		int nonzeronum=0;
		for(int i=starindex;i<endindex;i++)
		{
			if(c_Fconv2[i].x !=0)
				nonzeronum++;
			if(i<10)
			printf("%f ",c_Fconv2[i].x);
		}
		printf("nonzeronum block1  : %d from rank %d\n",nonzeronum,ig);

		nonzeronum=0;
		starindex=(fullsize/2);
		endindex = fullsize;
		for(int i=starindex;i<endindex;i++)
		{
			if(c_Fconv2[i].x !=0)
				nonzeronum++;
		}
		printf("nonzeronum block2 : %d from rank %d\n",nonzeronum,ig);
	}

	mulit_alltoall_one(dataplan,padoridim,padoridim,padoridim,0,offsetz);
	multi_sync(dataplan,GPU_N);
	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(dataplan[i].devicenum);
		cufftExecC2C(zplan[i], dataplan[i].d_Data + fftoffset[i],dataplan[i].d_Data + fftoffset[i], CUFFT_INVERSE);
	}
	multi_sync(dataplan,GPU_N);
	mulit_alltoall_two(dataplan,padoridim,padoridim,padoridim,0,offsetz);
	multi_memcpy_databack(dataplan,c_Fconv2,GPU_N,padoridim,padoridim);




	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(dataplan[i].devicenum);
		cufftDestroy(zplan[i]);
		cufftDestroy(xyplan[i]);
		cudaFree(dataplan[i].d_Data);
		cudaDeviceSynchronize();
	}
	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(dataplan[i].devicenum);
		size_t freedata1,total1;
		cudaMemGetInfo( &freedata1, &total1 );
		printf("After alloccation  : %ld   %ld and gpu num %d \n",freedata1,total1,i);
	}



//	cudaMemcpy(dataplan[0].d_Data,c_Fconv,padoridim *padoridim*padoridim *sizeof(cufftComplex),cudaMemcpyHostToDevice);
//	cufftPlan3d( &cuffthandle2, padoridim, padoridim, padoridim , CUFFT_C2C);
//	cufftExecC2C(cuffthandle2, dataplan[0].d_Data, dataplan[0].d_Data, CUFFT_INVERSE);
	//cudaMemcpy(c_Fconv,dataplan[0].d_Data,padoridim *padoridim*padoridim *sizeof(cufftComplex),cudaMemcpyDeviceToHost);
    for(int i=0;i<padoridim *padoridim*padoridim;i++)
    	vol_out.data[i]=c_Fconv2[i].x;

    printf("FINAL:");
    for(int i=0;i<10;i++)
    	printf("%f ",vol_out.data[i]);


    free(c_Fconv);
    cudaFreeHost(c_Fconv2);
/*
 *  method 1
	cufftReal *d_Freal;
	cudaMalloc((void**) &d_Freal, sizeof(cufftReal)*padoridim*padoridim*padoridim);
	printf("Before %d \n",Fconv.nzyxdim);
	windowFourierTransform(Fconv, padoridim);
	printf("After %d \n",Fconv.nzyxdim);
	cudaMemcpy(d_Fconv,Fconv.data,Fconv.nzyxdim *sizeof(cufftComplex),cudaMemcpyHostToDevice);
	cufftPlan3d( &cufftHandle2, padoridim, padoridim, padoridim , CUFFT_C2R);
	cufftExecC2R(cufftHandle2, d_Fconv, d_Freal);
	vol_out.reshape(padoridim, padoridim, padoridim);
	vol_out.setXmippOrigin();
	cudaMemcpy(vol_out.data,d_Freal,vol_out.nzyxdim *sizeof(cufftReal),cudaMemcpyDeviceToHost);
	*/


    CenterFFT(vol_out,true);

	// Window in real-space
	if (ref_dim==2)
	{
		vol_out.window(FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size),
				       LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size));
	}
	else
	{
		vol_out.window(FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size),
				       LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size));
	}
	vol_out.setXmippOrigin();

	// Normalisation factor of FFTW
	// The Fourier Transforms are all "normalised" for 2D transforms of size = ori_size x ori_size
	float normfft = (RFLOAT)(padding_factor * padding_factor * padding_factor * ori_size);
	vol_out /= normfft;
	// Mask out corners to prevent aliasing artefacts
	softMaskOutsideMap(vol_out);
	//windowToOridimRealSpace(transformer, vol_out, nr_threads, printTimes);
	RCTOCREC(ReconTimer,ReconS_17);


#endif

#ifdef DEBUG_RECONSTRUCT
	ttt()=vol_out;
	ttt.write("reconstruct_before_gridding_correction.spi");
#endif

	// Correct for the linear/nearest-neighbour interpolation that led to the data array
	RCTICREC(ReconTimer,ReconS_18);
	griddingCorrect(vol_out);
	RCTOCREC(ReconTimer,ReconS_18);
	// If the tau-values were calculated based on the FSC, then now re-calculate the power spectrum of the actual reconstruction
	if (update_tau2_with_fsc)
	{

		// New tau2 will be the power spectrum of the new map
		MultidimArray<RFLOAT> spectrum, count;

		// Calculate this map's power spectrum
		// Don't call getSpectrum() because we want to use the same transformer object to prevent memory trouble....
		RCTICREC(ReconTimer,ReconS_19);
		spectrum.initZeros(XSIZE(vol_out));
	    count.initZeros(XSIZE(vol_out));
		RCTOCREC(ReconTimer,ReconS_19);
		RCTICREC(ReconTimer,ReconS_20);
	    // recycle the same transformer for all images
        transformer.setReal(vol_out);
		RCTOCREC(ReconTimer,ReconS_20);
		RCTICREC(ReconTimer,ReconS_21);
        transformer.FourierTransform();
		RCTOCREC(ReconTimer,ReconS_21);
		RCTICREC(ReconTimer,ReconS_22);
	    FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
	    {
	    	long int idx = ROUND(sqrt(kp*kp + ip*ip + jp*jp));
	    	spectrum(idx) += norm(dAkij(Fconv, k, i, j));
	        count(idx) += 1.;
	    }
	    spectrum /= count;

		// Factor two because of two-dimensionality of the complex plane
		// (just like sigma2_noise estimates, the power spectra should be divided by 2)
		RFLOAT normfft = (ref_dim == 3 && data_dim == 2) ? (RFLOAT)(ori_size * ori_size) : 1.;
		spectrum *= normfft / 2.;

		// New SNR^MAP will be power spectrum divided by the noise in the reconstruction (i.e. sigma2)
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(data_vs_prior)
		{
			DIRECT_MULTIDIM_ELEM(tau2, n) =  tau2_fudge * DIRECT_MULTIDIM_ELEM(spectrum, n);
		}
		RCTOCREC(ReconTimer,ReconS_22);
	}
	RCTICREC(ReconTimer,ReconS_23);
	// Completely empty the transformer object
	transformer.cleanup();
    // Now can use extra mem to move data into smaller array space
    vol_out.shrinkToFit();

	RCTOCREC(ReconTimer,ReconS_23);
#ifdef TIMINGREC
    if(printTimes)
    	ReconTimer.printTimes(true);
#endif


#ifdef DEBUG_RECONSTRUCT
    std::cerr<<"done with reconstruct"<<std::endl;
#endif

	tau2_io = tau2;
    sigma2_out = sigma2;
    data_vs_prior_out = data_vs_prior;
    fourier_coverage_out = fourier_coverage;
}
void BackProjector::reconstruct_gpu(MultidimArray<RFLOAT> &vol_out,
                                int max_iter_preweight,
                                bool do_map,
                                RFLOAT tau2_fudge,
                                MultidimArray<RFLOAT> &tau2_io, // can be input/output
                                MultidimArray<RFLOAT> &sigma2_out,
                                MultidimArray<RFLOAT> &data_vs_prior_out,
                                MultidimArray<RFLOAT> &fourier_coverage_out,
                                const MultidimArray<RFLOAT> &fsc, // only input
                                RFLOAT normalise,
                                bool update_tau2_with_fsc,
                                bool is_whole_instead_of_half,
                                int nr_threads,
                                int minres_map,
                                bool printTimes,
								bool do_fsc0999)
{

#ifdef TIMINGREC
	Timer ReconTimer;
	printTimes=1;
	int ReconS_1 = ReconTimer.setNew(" RcS1_Init ");
	int ReconS_2 = ReconTimer.setNew(" RcS2_Shape&Noise ");
	int ReconS_2_5 = ReconTimer.setNew(" RcS2.5_Regularize ");
	int ReconS_3 = ReconTimer.setNew(" RcS3_skipGridding ");
	int ReconS_4 = ReconTimer.setNew(" RcS4_doGridding_norm ");
	int ReconS_5 = ReconTimer.setNew(" RcS5_doGridding_init ");
	int ReconS_6 = ReconTimer.setNew(" RcS6_doGridding_iter ");
	int ReconS_7 = ReconTimer.setNew(" RcS7_doGridding_apply ");
	int ReconS_8 = ReconTimer.setNew(" RcS8_blobConvolute ");
	int ReconS_9 = ReconTimer.setNew(" RcS9_blobResize ");
	int ReconS_10 = ReconTimer.setNew(" RcS10_blobSetReal ");
	int ReconS_11 = ReconTimer.setNew(" RcS11_blobSetTemp ");
	int ReconS_12 = ReconTimer.setNew(" RcS12_blobTransform ");
	int ReconS_13 = ReconTimer.setNew(" RcS13_blobCenterFFT ");
	int ReconS_14 = ReconTimer.setNew(" RcS14_blobNorm1 ");
	int ReconS_15 = ReconTimer.setNew(" RcS15_blobSoftMask ");
	int ReconS_16 = ReconTimer.setNew(" RcS16_blobNorm2 ");
	int ReconS_17 = ReconTimer.setNew(" RcS17_WindowReal ");
	int ReconS_18 = ReconTimer.setNew(" RcS18_GriddingCorrect ");
	int ReconS_19 = ReconTimer.setNew(" RcS19_tauInit ");
	int ReconS_20 = ReconTimer.setNew(" RcS20_tausetReal ");
	int ReconS_21 = ReconTimer.setNew(" RcS21_tauTransform ");
	int ReconS_22 = ReconTimer.setNew(" RcS22_tautauRest ");
	int ReconS_23 = ReconTimer.setNew(" RcS23_tauShrinkToFit ");
	int ReconS_24 = ReconTimer.setNew(" RcS24_extra ");
#endif

    // never rely on references (handed to you from the outside) for computation:
    // they could be the same (i.e. reconstruct(..., dummy, dummy, dummy, dummy, ...); )

    MultidimArray<RFLOAT> sigma2, data_vs_prior, fourier_coverage;
	MultidimArray<RFLOAT> tau2 = tau2_io;


    RCTICREC(ReconTimer,ReconS_1);
    FourierTransformer transformer;
	MultidimArray<RFLOAT> Fweight;
	// Fnewweight can become too large for a float: always keep this one in double-precision
	MultidimArray<double> Fnewweight;
	MultidimArray<Complex>& Fconv = transformer.getFourierReference();
	int max_r2 = ROUND(r_max * padding_factor) * ROUND(r_max * padding_factor);

//#define DEBUG_RECONSTRUCT
#ifdef DEBUG_RECONSTRUCT
	Image<RFLOAT> ttt;
	FileName fnttt;
	ttt()=weight;
	ttt.write("reconstruct_initial_weight.spi");
	std::cerr << " pad_size= " << pad_size << " padding_factor= " << padding_factor << " max_r2= " << max_r2 << std::endl;
#endif

    // Set Fweight, Fnewweight and Fconv to the right size
    if (ref_dim == 2)
        vol_out.setDimensions(pad_size, pad_size, 1, 1);
    else
        // Too costly to actually allocate the space
        // Trick transformer with the right dimensions
        vol_out.setDimensions(pad_size, pad_size, pad_size, 1);

    transformer.setReal(vol_out); // Fake set real. 1. Allocate space for Fconv 2. calculate plans.
    vol_out.clear(); // Reset dimensions to 0

    RCTOCREC(ReconTimer,ReconS_1);
    RCTICREC(ReconTimer,ReconS_2);

    Fweight.reshape(Fconv);
    if (!skip_gridding)
    	Fnewweight.reshape(Fconv);

	// Go from projector-centered to FFTW-uncentered
	decenter(weight, Fweight, max_r2);

	// Take oversampling into account
	RFLOAT oversampling_correction = (ref_dim == 3) ? (padding_factor * padding_factor * padding_factor) : (padding_factor * padding_factor);
	MultidimArray<RFLOAT> counter;

	// First calculate the radial average of the (inverse of the) power of the noise in the reconstruction
	// This is the left-hand side term in the nominator of the Wiener-filter-like update formula
	// and it is stored inside the weight vector
	// Then, if (do_map) add the inverse of tau2-spectrum values to the weight
	sigma2.initZeros(ori_size/2 + 1);
	counter.initZeros(ori_size/2 + 1);
	FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
	{
		int r2 = kp * kp + ip * ip + jp * jp;
		if (r2 < max_r2)
		{
			int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
			RFLOAT invw = oversampling_correction * DIRECT_A3D_ELEM(Fweight, k, i, j);
			DIRECT_A1D_ELEM(sigma2, ires) += invw;
			DIRECT_A1D_ELEM(counter, ires) += 1.;
		}
    }

	// Average (inverse of) sigma2 in reconstruction
	FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(sigma2)
	{
        if (DIRECT_A1D_ELEM(sigma2, i) > 1e-10)
            DIRECT_A1D_ELEM(sigma2, i) = DIRECT_A1D_ELEM(counter, i) / DIRECT_A1D_ELEM(sigma2, i);
        else if (DIRECT_A1D_ELEM(sigma2, i) == 0)
            DIRECT_A1D_ELEM(sigma2, i) = 0.;
		else
		{
			std::cerr << " DIRECT_A1D_ELEM(sigma2, i)= " << DIRECT_A1D_ELEM(sigma2, i) << std::endl;
			REPORT_ERROR("BackProjector::reconstruct: ERROR: unexpectedly small, yet non-zero sigma2 value, this should not happen...a");
        }
    }

	if (update_tau2_with_fsc)
    {
        tau2.reshape(ori_size/2 + 1);
        data_vs_prior.initZeros(ori_size/2 + 1);
		// Then calculate new tau2 values, based on the FSC
		if (!fsc.sameShape(sigma2) || !fsc.sameShape(tau2))
		{
			fsc.printShape(std::cerr);
			tau2.printShape(std::cerr);
			sigma2.printShape(std::cerr);
			REPORT_ERROR("ERROR BackProjector::reconstruct: sigma2, tau2 and fsc have different sizes");
		}
		FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(sigma2)
        {
			// FSC cannot be negative or zero for conversion into tau2
			RFLOAT myfsc = XMIPP_MAX(0.001, DIRECT_A1D_ELEM(fsc, i));
			if (is_whole_instead_of_half)
			{
				// Factor two because of twice as many particles
				// Sqrt-term to get 60-degree phase errors....
				myfsc = sqrt(2. * myfsc / (myfsc + 1.));
			}
			myfsc = XMIPP_MIN(0.999, myfsc);
			RFLOAT myssnr = myfsc / (1. - myfsc);
			// Sjors 29nov2017 try tau2_fudge for pulling harder on Refine3D runs...
            myssnr *= tau2_fudge;
			RFLOAT fsc_based_tau = myssnr * DIRECT_A1D_ELEM(sigma2, i);
			DIRECT_A1D_ELEM(tau2, i) = fsc_based_tau;
			// data_vs_prior is merely for reporting: it is not used for anything in the reconstruction
			DIRECT_A1D_ELEM(data_vs_prior, i) = myssnr;
		}
	}
    RCTOCREC(ReconTimer,ReconS_2);
    RCTICREC(ReconTimer,ReconS_2_5);
	// Apply MAP-additional term to the Fnewweight array
	// This will regularise the actual reconstruction
    if (do_map)
	{

    	// Then, add the inverse of tau2-spectrum values to the weight
		// and also calculate spherical average of data_vs_prior ratios
		if (!update_tau2_with_fsc)
			data_vs_prior.initZeros(ori_size/2 + 1);
		fourier_coverage.initZeros(ori_size/2 + 1);
		counter.initZeros(ori_size/2 + 1);
		FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
 		{
			int r2 = kp * kp + ip * ip + jp * jp;
			if (r2 < max_r2)
			{
				int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
				RFLOAT invw = DIRECT_A3D_ELEM(Fweight, k, i, j);

				RFLOAT invtau2;
				if (DIRECT_A1D_ELEM(tau2, ires) > 0.)
				{
					// Calculate inverse of tau2
					invtau2 = 1. / (oversampling_correction * tau2_fudge * DIRECT_A1D_ELEM(tau2, ires));
				}
				else if (DIRECT_A1D_ELEM(tau2, ires) == 0.)
				{
					// If tau2 is zero, use small value instead
					invtau2 = 1./ ( 0.001 * invw);
				}
				else
				{
					std::cerr << " sigma2= " << sigma2 << std::endl;
					std::cerr << " fsc= " << fsc << std::endl;
					std::cerr << " tau2= " << tau2 << std::endl;
					REPORT_ERROR("ERROR BackProjector::reconstruct: Negative or zero values encountered for tau2 spectrum!");
				}

				// Keep track of spectral evidence-to-prior ratio and remaining noise in the reconstruction
				if (!update_tau2_with_fsc)
					DIRECT_A1D_ELEM(data_vs_prior, ires) += invw / invtau2;

				// Keep track of the coverage in Fourier space
				if (invw / invtau2 >= 1.)
					DIRECT_A1D_ELEM(fourier_coverage, ires) += 1.;

				DIRECT_A1D_ELEM(counter, ires) += 1.;

				// Only for (ires >= minres_map) add Wiener-filter like term
				if (ires >= minres_map)
				{
					// Now add the inverse-of-tau2_class term
					invw += invtau2;
					// Store the new weight again in Fweight
					DIRECT_A3D_ELEM(Fweight, k, i, j) = invw;
				}
			}
		}

		// Average data_vs_prior
		if (!update_tau2_with_fsc)
		{
			FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(data_vs_prior)
			{
				if (i > r_max)
					DIRECT_A1D_ELEM(data_vs_prior, i) = 0.;
				else if (DIRECT_A1D_ELEM(counter, i) < 0.001)
					DIRECT_A1D_ELEM(data_vs_prior, i) = 999.;
				else
					DIRECT_A1D_ELEM(data_vs_prior, i) /= DIRECT_A1D_ELEM(counter, i);
			}
		}

		// Calculate Fourier coverage in each shell
		FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(fourier_coverage)
		{
			if (DIRECT_A1D_ELEM(counter, i) > 0.)
				DIRECT_A1D_ELEM(fourier_coverage, i) /= DIRECT_A1D_ELEM(counter, i);
		}

	} //end if do_map
    else if (do_fsc0999)
    {

     	// Sjors 9may2018: avoid numerical instabilities with unregularised reconstructions....
        FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
        {
            int r2 = kp * kp + ip * ip + jp * jp;
            if (r2 < max_r2)
            {
                int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
                if (ires >= minres_map)
                {
                    // add 1/1000th of the radially averaged sigma2 to the Fweight, to avoid having zeros there...
                	DIRECT_A3D_ELEM(Fweight, k, i, j) += 1./(999. * DIRECT_A1D_ELEM(sigma2, ires));
                }
            }
        }

    }

//========================reconstruct_gpu  =============================add  GPU version

	int Ndim[3];
	int GPU_N=4;
	Ndim[0]=pad_size;
	Ndim[1]=pad_size;
	Ndim[2]=pad_size;
	size_t fullsize= pad_size*pad_size*pad_size;
    initgpu(GPU_N);
	RFLOAT *d_Fweight;
	double *d_Fnewweight;
	int offset;

	int *numberZ = (int *)malloc(sizeof(int)*GPU_N);
	int *offsetZ = (int *)malloc(sizeof(int)*GPU_N);
	dividetask(numberZ,offsetZ,pad_size,GPU_N);


	MultiGPUplan *dataplan;
	dataplan = (MultiGPUplan *)malloc(sizeof(MultiGPUplan)*GPU_N);
	//multi_plan_init_transpose(plan,GPU_N,numberZ,offsetZ,pad_size);
	multi_plan_init(dataplan,GPU_N,fullsize,numberZ,offsetZ,pad_size);

	for (int i = 0; i < GPU_N; ++i) {
		cudaSetDevice(dataplan[i].devicenum);
		cudaMalloc((void**) &(dataplan[i].d_Data),sizeof(cufftComplex) * dataplan[i].datasize);
	}

	//=================================
	int xyN[2];
	xyN[0] = Ndim[0];
	xyN[1] = Ndim[1];

	cufftHandle *xyplan;
	xyplan = (cufftHandle*) malloc(sizeof(cufftHandle)*GPU_N);
	cufftHandle *zplan;
	zplan = (cufftHandle*) malloc(sizeof(cufftHandle)*GPU_N);

//	int fftoffset[2];
//	fftoffset[0] = 0;
//	fftoffset[1] = Ndim[0] * (Ndim[1] / 2);
	//===================================


    RCTOCREC(ReconTimer,ReconS_2_5);
	if (skip_gridding)
	{
	    RCTICREC(ReconTimer,ReconS_3);
		std::cerr << "Skipping gridding!" << std::endl;
		Fconv.initZeros(); // to remove any stuff from the input volume
		decenter(data, Fconv, max_r2);

		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			if (DIRECT_MULTIDIM_ELEM(Fweight, n) > 0.)
				DIRECT_MULTIDIM_ELEM(Fconv, n) /= DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		RCTOCREC(ReconTimer,ReconS_3);
#ifdef DEBUG_RECONSTRUCT
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			DIRECT_MULTIDIM_ELEM(ttt(), n) = DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		ttt.write("reconstruct_skipgridding_correction_term.spi");
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			if (DIRECT_MULTIDIM_ELEM(Fweight, n) > 0.)
				DIRECT_MULTIDIM_ELEM(ttt(), n) = 1./DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		ttt.write("reconstruct_skipgridding_correction_term_inverse.spi");
#endif
	}
	else
	{
		RCTICREC(ReconTimer,ReconS_4);
		// Divide both data and Fweight by normalisation factor to prevent FFT's with very large values....
	#ifdef DEBUG_RECONSTRUCT
		std::cerr << " normalise= " << normalise << std::endl;
	#endif
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fweight)
		{
			DIRECT_MULTIDIM_ELEM(Fweight, n) /= normalise;
		}
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(data)
		{
			DIRECT_MULTIDIM_ELEM(data, n) /= normalise;
		}
		RCTOCREC(ReconTimer,ReconS_4);
		RCTICREC(ReconTimer,ReconS_5);
        // Initialise Fnewweight with 1's and 0's. (also see comments below)
		FOR_ALL_ELEMENTS_IN_ARRAY3D(weight)
		{
			if (k * k + i * i + j * j < max_r2)
				A3D_ELEM(weight, k, i, j) = 1.;
			else
				A3D_ELEM(weight, k, i, j) = 0.;
		}



		decenter(weight, Fnewweight, max_r2);
		RCTOCREC(ReconTimer,ReconS_5);
		// Iterative algorithm as in  Eq. [14] in Pipe & Menon (1999)
		// or Eq. (4) in Matej (2001)



		long int Fconvnum=Fconv.nzyxdim;
		long int normsize= Ndim[0]*Ndim[1]*Ndim[2];
		int halfxdim=Fconv.xdim;

		printf("%ld %ld %ld \n",Fconv.xdim,Fconv.ydim,Fconv.zdim);
		printf("Fnewweight: %ld %ld %ld \n",Fnewweight.xdim,Fnewweight.ydim,Fnewweight.zdim);


		multi_enable_access(dataplan,GPU_N);



         //==================2d fft plan

		for (int i = 0; i < GPU_N; i++) {
		    cudaSetDevice(dataplan[i].devicenum);
		    cufftPlanMany(&xyplan[i], 2, xyN, NULL, 0, 0, NULL, 0, 0, CUFFT_C2C, numberZ[i]);
		}
	    //==================1d fft plan

		for (int i = 0; i < GPU_N; i++) {
		    cudaSetDevice(dataplan[i].devicenum);
			int rank=1;
			int nrank[1];
			nrank[0]=Ndim[1];
			int inembed[3];
			inembed[0] = Ndim[0];inembed[1] = numberZ[i];inembed[2] = Ndim[2];
			cufftPlanMany(&zplan[i], rank, nrank, inembed, Ndim[0] * Ndim[1] , 1, inembed, Ndim[0] * Ndim[1], 1, CUFFT_C2C, Ndim[0] * (numberZ[i]));
		}


		float *fweightdata= (float *)malloc(sizeof(float)*pad_size*pad_size*pad_size);
		layoutchange(Fweight.data,Fweight.xdim,Fweight.ydim,Fweight.zdim,pad_size,fweightdata);
		float **d_blocktwo;
		d_blocktwo =(float **)malloc(GPU_N*sizeof(float*));
		for(int i=0;i< GPU_N;i++)
		{
			cudaSetDevice(dataplan[i].devicenum);
			cudaMalloc((void**) &(d_blocktwo[i]),sizeof(float) * dataplan[i].realsize);
			cudaMemcpy(d_blocktwo[i],fweightdata+dataplan[i].selfoffset,dataplan[i].realsize*sizeof(float),cudaMemcpyHostToDevice);
		}
		multi_sync(dataplan,GPU_N);
		free(fweightdata);


		double *tempdata= (double *)malloc(sizeof(double)*pad_size*pad_size*pad_size);
		// every thread map two block to used
		layoutchange(Fnewweight.data,Fnewweight.xdim,Fnewweight.ydim,Fnewweight.zdim,pad_size,tempdata);
		double **d_blockone;
		d_blockone = (double **)malloc(GPU_N*sizeof(double *));

		for (int i = 0; i < GPU_N; i++) {
		    cudaSetDevice(dataplan[i].devicenum);
			cudaMalloc((void**) &(d_blockone[i]),sizeof(double) * dataplan[i].realsize);
			cudaMemcpy(d_blockone[i],tempdata+dataplan[i].selfoffset,dataplan[i].realsize*sizeof(double),cudaMemcpyHostToDevice);
		}
		multi_sync(dataplan,GPU_N);
		struct timeval tv1,tv2;
		struct timezone tz;

		float time_use=0;





		RFLOAT **d_tab_ftblob;
		for (int iter = 0; iter < max_iter_preweight; iter++)
		{

            //std::cout << "    iteration " << (iter+1) << "/" << max_iter_preweight << "\n";
			RCTICREC(ReconTimer,ReconS_6);


			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(dataplan[i].devicenum);
				vector_Multi_layout_mpi(d_blockone[i],d_blocktwo[i],dataplan[i].d_Data+dataplan[i].selfoffset,dataplan[i].realsize);
				cufftExecC2C(xyplan[i], dataplan[i].d_Data + dataplan[i].selfoffset,dataplan[i].d_Data + dataplan[i].selfoffset, CUFFT_INVERSE);
				cudaDeviceSynchronize();
			}
			gettimeofday (&tv1, &tz);

			gpu_alltoall_multinode(dataplan,GPU_N,pad_size,offsetZ);
			gettimeofday (&tv2, &tz);
				time_use +=1000 * (tv2.tv_sec-tv1.tv_sec)+ (tv2.tv_usec-tv1.tv_usec)/1000;
			multi_sync(dataplan,GPU_N);
			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(dataplan[i].devicenum);
				cufftExecC2C(zplan[i], dataplan[i].d_Data+(offsetZ[i]*pad_size) ,dataplan[i].d_Data+(offsetZ[i]*pad_size) , CUFFT_INVERSE);

			}

			multi_sync(dataplan,GPU_N);

			d_tab_ftblob = (RFLOAT **) malloc(sizeof(RFLOAT *)*GPU_N);
			for(int i=0;i< GPU_N;i++)
			{
				cudaSetDevice(dataplan[i].devicenum);
#ifdef RELION_SINGLE_PRECISION
				d_tab_ftblob[i]=gpusetdata_float(d_tab_ftblob[i],tab_ftblob.tabulatedValues.xdim,tab_ftblob.tabulatedValues.data);
#else
				d_tab_ftblob[i]=gpusetdata_double(d_tab_ftblob[i],tab_ftblob.tabulatedValues.xdim,tab_ftblob.tabulatedValues.data);
#endif
			}


			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(dataplan[i].devicenum);
				RFLOAT normftblob = tab_ftblob(0.);
				int tabxdim=tab_ftblob.tabulatedValues.xdim;
				volume_Multi_float_mpi(dataplan[i].d_Data,d_tab_ftblob[i], Ndim[0]*numberZ[i]*Ndim[2],
												tabxdim, tab_ftblob.sampling , pad_size/2, pad_size, ori_size, padding_factor, normftblob,
												numberZ[i],offsetZ[i]);
			}
			multi_sync(dataplan,GPU_N);
			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(dataplan[i].devicenum);
				cufftExecC2C(zplan[i], dataplan[i].d_Data + (offsetZ[i]*pad_size),dataplan[i].d_Data + (offsetZ[i]*pad_size), CUFFT_FORWARD);
				cudaDeviceSynchronize();
			}
			gettimeofday (&tv1, &tz);

			gpu_alltoall_multinode_inverse(dataplan,GPU_N,pad_size,offsetZ);
			gettimeofday (&tv2, &tz);
						time_use +=1000 * (tv2.tv_sec-tv1.tv_sec)+ (tv2.tv_usec-tv1.tv_usec)/1000;
//=========================EXE CUFFT_FORWARD

			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(dataplan[i].devicenum);
				cufftExecC2C(xyplan[i], dataplan[i].d_Data + dataplan[i].selfoffset,dataplan[i].d_Data + dataplan[i].selfoffset, CUFFT_FORWARD);
				cudaDeviceSynchronize();
			}


			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(dataplan[i].devicenum);
				vector_Normlize(dataplan[i].d_Data, normsize ,Ndim[0]*Ndim[1]*Ndim[2]);
				//gpu_kernel3
				fft_Divide_mpi(dataplan[i].d_Data+dataplan[i].selfoffset,d_blockone[i],dataplan[i].realsize,
									pad_size*pad_size,pad_size,pad_size,pad_size,Fconv.xdim,max_r2,offsetZ[i]);

			}
			multi_sync(dataplan,GPU_N);


			RCTOCREC(ReconTimer,ReconS_6);
			//cudaMemcpy(c_Fconv,dataplan[0].d_Data,100*sizeof(cufftComplex),cudaMemcpyDeviceToHost);
			//printf("step iter end  from gpu : \n");

	#ifdef DEBUG_RECONSTRUCT
			std::cerr << " PREWEIGHTING ITERATION: "<< iter + 1 << " OF " << max_iter_preweight << std::endl;
			// report of maximum and minimum values of current conv_weight
			std::cerr << " corr_avg= " << corr_avg / corr_nn << std::endl;
			std::cerr << " corr_min= " << corr_min << std::endl;
			std::cerr << " corr_max= " << corr_max << std::endl;
	#endif

		}
		for(int i=0;i<GPU_N; i++){
			cudaSetDevice(dataplan[i].devicenum);
			cudaMemcpy(tempdata+dataplan[i].selfoffset,d_blockone[i],dataplan[i].realsize*sizeof(double),cudaMemcpyDeviceToHost);
		}
		multi_sync(dataplan,GPU_N);
		layoutchangeback(tempdata,Fnewweight.xdim,Fnewweight.ydim,Fnewweight.zdim,pad_size,Fnewweight.data);
		printf("\ncpy time : %f \n",time_use);
/*
		double sum=0;
		printf("TEMP res :\n");
		for(int i=0;i<Fnewweight.xdim*Fnewweight.ydim*Fnewweight.zdim;i++)
		{
			if(i<10)
				printf("%f ",Fnewweight.data[i]);
			sum+=Fnewweight.data[i];
		}
		printf("sum : %f \n",sum);*/
		//cudaMemcpy(Fnewweight.data,d_Fnewweight,Fnewweight.nzyxdim*sizeof(double),cudaMemcpyDeviceToHost);

		for (int i = 0; i < GPU_N; i++) {
			cudaSetDevice(dataplan[i].devicenum);
			cufftDestroy(xyplan[i]);
			cufftDestroy(zplan[i]);
			cudaFree(d_blockone[i]);
			cudaFree(d_blocktwo[i]);
			cudaFree(d_tab_ftblob[i]);
		}
		free(tempdata);
		free(d_blockone);
		free(d_blocktwo);
		free(d_tab_ftblob);
		RCTICREC(ReconTimer,ReconS_7);

	#ifdef DEBUG_RECONSTRUCT
		Image<double> tttt;
		tttt()=Fnewweight;
		tttt.write("reconstruct_gridding_weight.spi");
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			DIRECT_MULTIDIM_ELEM(ttt(), n) = abs(DIRECT_MULTIDIM_ELEM(Fconv, n));
		}
		ttt.write("reconstruct_gridding_correction_term.spi");
	#endif


		// Clear memory
		Fweight.clear();

		// Note that Fnewweight now holds the approximation of the inverse of the weights on a regular grid

		// Now do the actual reconstruction with the data array
		// Apply the iteratively determined weight
		Fconv.initZeros(); // to remove any stuff from the input volume
		decenter(data, Fconv, max_r2);
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
#ifdef  RELION_SINGLE_PRECISION
			// Prevent numerical instabilities in single-precision reconstruction with very unevenly sampled orientations
			if (DIRECT_MULTIDIM_ELEM(Fnewweight, n) > 1e20)
				DIRECT_MULTIDIM_ELEM(Fnewweight, n) = 1e20;
#endif
			DIRECT_MULTIDIM_ELEM(Fconv, n) *= DIRECT_MULTIDIM_ELEM(Fnewweight, n);
		}

		// Clear memory
		Fnewweight.clear();
		RCTOCREC(ReconTimer,ReconS_7);
	} // end if skip_gridding

// Gridding theory says one now has to interpolate the fine grid onto the coarse one using a blob kernel
// and then do the inverse transform and divide by the FT of the blob (i.e. do the gridding correction)
// In practice, this gives all types of artefacts (perhaps I never found the right implementation?!)
// Therefore, window the Fourier transform and then do the inverse transform
//#define RECONSTRUCT_CONVOLUTE_BLOB
#ifdef RECONSTRUCT_CONVOLUTE_BLOB

	// Apply the same blob-convolution as above to the data array
	// Mask real-space map beyond its original size to prevent aliasing in the downsampling step below
	RCTICREC(ReconTimer,ReconS_8);
	convoluteBlobRealSpace(transformer, true);
	RCTOCREC(ReconTimer,ReconS_8);
	RCTICREC(ReconTimer,ReconS_9);
	// Now just pick every 3rd pixel in Fourier-space (i.e. down-sample)
	// and do a final inverse FT
	if (ref_dim == 2)
		vol_out.resize(ori_size, ori_size);
	else
		vol_out.resize(ori_size, ori_size, ori_size);
	RCTOCREC(ReconTimer,ReconS_9);
	RCTICREC(ReconTimer,ReconS_10);
	FourierTransformer transformer2;
	MultidimArray<Complex > Ftmp;
	transformer2.setReal(vol_out); // cannot use the first transformer because Fconv is inside there!!
	transformer2.getFourierAlias(Ftmp);
	RCTOCREC(ReconTimer,ReconS_10);
	RCTICREC(ReconTimer,ReconS_11);
	FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Ftmp)
	{
		if (kp * kp + ip * ip + jp * jp < r_max * r_max)
		{
			DIRECT_A3D_ELEM(Ftmp, k, i, j) = FFTW_ELEM(Fconv, kp * padding_factor, ip * padding_factor, jp * padding_factor);
		}
		else
		{
			DIRECT_A3D_ELEM(Ftmp, k, i, j) = 0.;
		}
	}
	RCTOCREC(ReconTimer,ReconS_11);
	RCTICREC(ReconTimer,ReconS_12);
	// inverse FFT leaves result in vol_out
	transformer2.inverseFourierTransform();
	RCTOCREC(ReconTimer,ReconS_12);
	RCTICREC(ReconTimer,ReconS_13);
	// Shift the map back to its origin
	CenterFFT(vol_out, false);
	RCTOCREC(ReconTimer,ReconS_13);
	RCTICREC(ReconTimer,ReconS_14);
	// Un-normalize FFTW (because original FFTs were done with the size of 2D FFTs)
	if (ref_dim==3)
		vol_out /= ori_size;
	RCTOCREC(ReconTimer,ReconS_14);
	RCTICREC(ReconTimer,ReconS_15);
	// Mask out corners to prevent aliasing artefacts
	softMaskOutsideMap(vol_out);
	RCTOCREC(ReconTimer,ReconS_15);
	RCTICREC(ReconTimer,ReconS_16);
	// Gridding correction for the blob
	RFLOAT normftblob = tab_ftblob(0.);
	FOR_ALL_ELEMENTS_IN_ARRAY3D(vol_out)
	{

		RFLOAT r = sqrt((RFLOAT)(k*k+i*i+j*j));
		RFLOAT rval = r / (ori_size * padding_factor);
		A3D_ELEM(vol_out, k, i, j) /= tab_ftblob(rval) / normftblob;
		//if (k==0 && i==0)
		//	std::cerr << " j= " << j << " rval= " << rval << " tab_ftblob(rval) / normftblob= " << tab_ftblob(rval) / normftblob << std::endl;
	}
	RCTOCREC(ReconTimer,ReconS_16);

#else

	// rather than doing the blob-convolution to downsample the data array, do a windowing operation:
	// This is the same as convolution with a SINC. It seems to give better maps.
	// Then just make the blob look as much as a SINC as possible....
	// The "standard" r1.9, m2 and a15 blob looks quite like a sinc until the first zero (perhaps that's why it is standard?)
	//for (RFLOAT r = 0.1; r < 10.; r+=0.01)
	//{
	//	RFLOAT sinc = sin(PI * r / padding_factor ) / ( PI * r / padding_factor);
	//	std::cout << " r= " << r << " sinc= " << sinc << " blob= " << blob_val(r, blob) << std::endl;
	//}

	// Now do inverse FFT and window to original size in real-space
	// Pass the transformer to prevent making and clearing a new one before clearing the one declared above....
	// The latter may give memory problems as detected by electric fence....
	RCTICREC(ReconTimer,ReconS_17);

	// Size of padded real-space volume
	int padoridim = ROUND(padding_factor * ori_size);
	// make sure padoridim is even
	padoridim += padoridim%2;
    vol_out.reshape(padoridim, padoridim, padoridim);
    vol_out.setXmippOrigin();
	fullsize = padoridim *padoridim*padoridim;

	dividetask(numberZ,offsetZ,padoridim,GPU_N);
	multi_plan_init(dataplan,GPU_N,fullsize,numberZ,offsetZ,padoridim);

	if(padoridim > pad_size)
	{
		for (int i = 0; i < GPU_N; ++i) {
			cudaSetDevice(dataplan[i].devicenum);
			cudaFree((dataplan[i].d_Data));
			cudaMalloc((void**) &(dataplan[i].d_Data),sizeof(cufftComplex) * dataplan[i].datasize);
		}
	}
	cufftComplex *c_Fconv;
	c_Fconv= (cufftComplex *)malloc(fullsize * sizeof(cufftComplex));
	windowFourierTransform(Fconv, padoridim);

//	printf("layoutchangecomp : %ld %ld %ld %d\n",Fconv.xdim,Fconv.ydim,Fconv.zdim,padoridim);
	layoutchangecomp(Fconv.data,Fconv.xdim,Fconv.ydim,Fconv.zdim,padoridim,c_Fconv);

//	printf("layoutchangecomp : %ld %ld %ld %d\n",Fconv.xdim,Fconv.ydim,Fconv.zdim,padoridim);
//init plan and for even data



	xyN[0] = padoridim;
	xyN[1] = padoridim;
    //==================2d fft plan

	for (int i = 0; i < GPU_N; i++) {
	    cudaSetDevice(dataplan[i].devicenum);
	    cufftPlanMany(&xyplan[i], 2, xyN, NULL, 0, 0, NULL, 0, 0, CUFFT_C2C, numberZ[i]);
	}
   //==================1d fft plan
	int Ncol[1];
	Ncol[0] = padoridim;

	for (int i = 0; i < GPU_N; i++) {
	    cudaSetDevice(dataplan[i].devicenum);
		int inembed[3];
		inembed[0] = padoridim;inembed[1] = dataplan[i].selfZ;inembed[2] = padoridim;
	    cufftPlanMany(&zplan[i], 1, Ncol, inembed, padoridim * padoridim, 1,inembed, padoridim * padoridim, 1, CUFFT_C2C, padoridim * (dataplan[i].selfZ));
	    cudaMemcpy(dataplan[i].d_Data + dataplan[i].selfoffset, c_Fconv + dataplan[i].selfoffset,
	    			(dataplan[i].selfZ * padoridim * padoridim) * sizeof(cufftComplex),cudaMemcpyHostToDevice);
	}

	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(dataplan[i].devicenum);
		cufftExecC2C(xyplan[i], dataplan[i].d_Data + dataplan[i].selfoffset,dataplan[i].d_Data + dataplan[i].selfoffset, CUFFT_INVERSE);
	}
	multi_sync(dataplan,GPU_N);
	gpu_alltoall_multinode(dataplan,GPU_N,padoridim,offsetZ);

	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(dataplan[i].devicenum);
		cufftExecC2C(zplan[i], dataplan[i].d_Data + (offsetZ[i]*padoridim),dataplan[i].d_Data + (offsetZ[i]*padoridim), CUFFT_INVERSE);
	}
	multi_sync(dataplan,GPU_N);
	//mulit_alltoall_two(dataplan,padoridim,padoridim,padoridim,0,offsetz);

	gpu_alltoall_multinode_inverse(dataplan,GPU_N,pad_size,offsetZ);
	//multi_memcpy_databack(dataplan,c_Fconv2,GPU_N,padoridim,padoridim);

	for(int i=0;i < GPU_N; i++)
	{
		cudaSetDevice(dataplan[i].devicenum);
		cudaMemcpy( c_Fconv + dataplan[i].selfoffset,dataplan[i].d_Data +  dataplan[i].selfoffset ,(dataplan[i].realsize) * sizeof(cufftComplex),cudaMemcpyDeviceToHost);
	}

	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(dataplan[i].devicenum);
		cufftDestroy(zplan[i]);
		cufftDestroy(xyplan[i]);
		cudaFree(dataplan[i].d_Data);
		cudaDeviceSynchronize();
	}
	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(dataplan[i].devicenum);
		size_t freedata1,total1;
		cudaMemGetInfo( &freedata1, &total1 );
		printf("After alloccation  : %ld   %ld and gpu num %d \n",freedata1,total1,i);
	}



//	cudaMemcpy(dataplan[0].d_Data,c_Fconv,padoridim *padoridim*padoridim *sizeof(cufftComplex),cudaMemcpyHostToDevice);
//	cufftPlan3d( &cuffthandle2, padoridim, padoridim, padoridim , CUFFT_C2C);
//	cufftExecC2C(cuffthandle2, dataplan[0].d_Data, dataplan[0].d_Data, CUFFT_INVERSE);
	//cudaMemcpy(c_Fconv,dataplan[0].d_Data,padoridim *padoridim*padoridim *sizeof(cufftComplex),cudaMemcpyDeviceToHost);
    for(int i=0;i<padoridim *padoridim*padoridim;i++)
    	vol_out.data[i]=c_Fconv[i].x;

    printf("FINAL:");
    for(int i=0;i<10;i++)
    	printf("%f ",vol_out.data[i]);


    free(c_Fconv);


    CenterFFT(vol_out,true);

	// Window in real-space
	if (ref_dim==2)
	{
		vol_out.window(FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size),
				       LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size));
	}
	else
	{
		vol_out.window(FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size),
				       LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size));
	}
	vol_out.setXmippOrigin();

	// Normalisation factor of FFTW
	// The Fourier Transforms are all "normalised" for 2D transforms of size = ori_size x ori_size
	float normfft = (RFLOAT)(padding_factor * padding_factor * padding_factor * ori_size);
	vol_out /= normfft;
	// Mask out corners to prevent aliasing artefacts
	softMaskOutsideMap(vol_out);
	//windowToOridimRealSpace(transformer, vol_out, nr_threads, printTimes);
	RCTOCREC(ReconTimer,ReconS_17);


#endif

#ifdef DEBUG_RECONSTRUCT
	ttt()=vol_out;
	ttt.write("reconstruct_before_gridding_correction.spi");
#endif

	// Correct for the linear/nearest-neighbour interpolation that led to the data array
	RCTICREC(ReconTimer,ReconS_18);
	griddingCorrect(vol_out);
	RCTOCREC(ReconTimer,ReconS_18);
	// If the tau-values were calculated based on the FSC, then now re-calculate the power spectrum of the actual reconstruction
	if (update_tau2_with_fsc)
	{

		// New tau2 will be the power spectrum of the new map
		MultidimArray<RFLOAT> spectrum, count;

		// Calculate this map's power spectrum
		// Don't call getSpectrum() because we want to use the same transformer object to prevent memory trouble....
		RCTICREC(ReconTimer,ReconS_19);
		spectrum.initZeros(XSIZE(vol_out));
	    count.initZeros(XSIZE(vol_out));
		RCTOCREC(ReconTimer,ReconS_19);
		RCTICREC(ReconTimer,ReconS_20);
	    // recycle the same transformer for all images
        transformer.setReal(vol_out);
		RCTOCREC(ReconTimer,ReconS_20);
		RCTICREC(ReconTimer,ReconS_21);
        transformer.FourierTransform();
		RCTOCREC(ReconTimer,ReconS_21);
		RCTICREC(ReconTimer,ReconS_22);
	    FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
	    {
	    	long int idx = ROUND(sqrt(kp*kp + ip*ip + jp*jp));
	    	spectrum(idx) += norm(dAkij(Fconv, k, i, j));
	        count(idx) += 1.;
	    }
	    spectrum /= count;

		// Factor two because of two-dimensionality of the complex plane
		// (just like sigma2_noise estimates, the power spectra should be divided by 2)
		RFLOAT normfft = (ref_dim == 3 && data_dim == 2) ? (RFLOAT)(ori_size * ori_size) : 1.;
		spectrum *= normfft / 2.;

		// New SNR^MAP will be power spectrum divided by the noise in the reconstruction (i.e. sigma2)
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(data_vs_prior)
		{
			DIRECT_MULTIDIM_ELEM(tau2, n) =  tau2_fudge * DIRECT_MULTIDIM_ELEM(spectrum, n);
		}
		RCTOCREC(ReconTimer,ReconS_22);
	}
	RCTICREC(ReconTimer,ReconS_23);
	// Completely empty the transformer object
	transformer.cleanup();
    // Now can use extra mem to move data into smaller array space
    vol_out.shrinkToFit();

	RCTOCREC(ReconTimer,ReconS_23);
#ifdef TIMINGREC
    if(printTimes)
    	ReconTimer.printTimes(true);
#endif


#ifdef DEBUG_RECONSTRUCT
    std::cerr<<"done with reconstruct"<<std::endl;
#endif

	tau2_io = tau2;
    sigma2_out = sigma2;
    data_vs_prior_out = data_vs_prior;
    fourier_coverage_out = fourier_coverage;
}

void BackProjector::reconstruct_gpu_single(MultidimArray<RFLOAT> &vol_out,
                                int max_iter_preweight,
                                bool do_map,
                                RFLOAT tau2_fudge,
                                MultidimArray<RFLOAT> &tau2_io, // can be input/output
                                MultidimArray<RFLOAT> &sigma2_out,
                                MultidimArray<RFLOAT> &data_vs_prior_out,
                                MultidimArray<RFLOAT> &fourier_coverage_out,
                                const MultidimArray<RFLOAT> &fsc, // only input
                                RFLOAT normalise,
                                bool update_tau2_with_fsc,
                                bool is_whole_instead_of_half,
                                int nr_threads,
                                int minres_map,
                                bool printTimes,
								bool do_fsc0999)
{
#ifdef TIMINGREC
	Timer ReconTimer;
	printTimes=1;
	int ReconS_1 = ReconTimer.setNew(" RcS1_Init ");
	int ReconS_2 = ReconTimer.setNew(" RcS2_Shape&Noise ");
	int ReconS_2_5 = ReconTimer.setNew(" RcS2.5_Regularize ");
	int ReconS_3 = ReconTimer.setNew(" RcS3_skipGridding ");
	int ReconS_4 = ReconTimer.setNew(" RcS4_doGridding_norm ");
	int ReconS_5 = ReconTimer.setNew(" RcS5_doGridding_init ");
	int ReconS_6 = ReconTimer.setNew(" RcS6_doGridding_iter ");
	int ReconS_7 = ReconTimer.setNew(" RcS7_doGridding_apply ");
	int ReconS_8 = ReconTimer.setNew(" RcS8_blobConvolute ");
	int ReconS_9 = ReconTimer.setNew(" RcS9_blobResize ");
	int ReconS_10 = ReconTimer.setNew(" RcS10_blobSetReal ");
	int ReconS_11 = ReconTimer.setNew(" RcS11_blobSetTemp ");
	int ReconS_12 = ReconTimer.setNew(" RcS12_blobTransform ");
	int ReconS_13 = ReconTimer.setNew(" RcS13_blobCenterFFT ");
	int ReconS_14 = ReconTimer.setNew(" RcS14_blobNorm1 ");
	int ReconS_15 = ReconTimer.setNew(" RcS15_blobSoftMask ");
	int ReconS_16 = ReconTimer.setNew(" RcS16_blobNorm2 ");
	int ReconS_17 = ReconTimer.setNew(" RcS17_WindowReal ");
	int ReconS_18 = ReconTimer.setNew(" RcS18_GriddingCorrect ");
	int ReconS_19 = ReconTimer.setNew(" RcS19_tauInit ");
	int ReconS_20 = ReconTimer.setNew(" RcS20_tausetReal ");
	int ReconS_21 = ReconTimer.setNew(" RcS21_tauTransform ");
	int ReconS_22 = ReconTimer.setNew(" RcS22_tautauRest ");
	int ReconS_23 = ReconTimer.setNew(" RcS23_tauShrinkToFit ");
	int ReconS_24 = ReconTimer.setNew(" RcS24_extra ");
#endif

    // never rely on references (handed to you from the outside) for computation:
    // they could be the same (i.e. reconstruct(..., dummy, dummy, dummy, dummy, ...); )

    MultidimArray<RFLOAT> sigma2, data_vs_prior, fourier_coverage;
	MultidimArray<RFLOAT> tau2 = tau2_io;


    RCTICREC(ReconTimer,ReconS_1);
    FourierTransformer transformer;
	MultidimArray<RFLOAT> Fweight;
	// Fnewweight can become too large for a float: always keep this one in double-precision
	MultidimArray<double> Fnewweight;
	MultidimArray<Complex>& Fconv = transformer.getFourierReference();
	int max_r2 = ROUND(r_max * padding_factor) * ROUND(r_max * padding_factor);

//#define DEBUG_RECONSTRUCT
#ifdef DEBUG_RECONSTRUCT
	Image<RFLOAT> ttt;
	FileName fnttt;
	ttt()=weight;
	ttt.write("reconstruct_initial_weight.spi");
	std::cerr << " pad_size= " << pad_size << " padding_factor= " << padding_factor << " max_r2= " << max_r2 << std::endl;
#endif

    // Set Fweight, Fnewweight and Fconv to the right size
    if (ref_dim == 2)
        vol_out.setDimensions(pad_size, pad_size, 1, 1);
    else
        // Too costly to actually allocate the space
        // Trick transformer with the right dimensions
        vol_out.setDimensions(pad_size, pad_size, pad_size, 1);

    transformer.setReal(vol_out); // Fake set real. 1. Allocate space for Fconv 2. calculate plans.
    vol_out.clear(); // Reset dimensions to 0

    RCTOCREC(ReconTimer,ReconS_1);
    RCTICREC(ReconTimer,ReconS_2);

    Fweight.reshape(Fconv);
    if (!skip_gridding)
    	Fnewweight.reshape(Fconv);

	// Go from projector-centered to FFTW-uncentered
	decenter(weight, Fweight, max_r2);

	// Take oversampling into account
	RFLOAT oversampling_correction = (ref_dim == 3) ? (padding_factor * padding_factor * padding_factor) : (padding_factor * padding_factor);
	MultidimArray<RFLOAT> counter;

	// First calculate the radial average of the (inverse of the) power of the noise in the reconstruction
	// This is the left-hand side term in the nominator of the Wiener-filter-like update formula
	// and it is stored inside the weight vector
	// Then, if (do_map) add the inverse of tau2-spectrum values to the weight
	sigma2.initZeros(ori_size/2 + 1);
	counter.initZeros(ori_size/2 + 1);
	FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
	{
		int r2 = kp * kp + ip * ip + jp * jp;
		if (r2 < max_r2)
		{
			int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
			RFLOAT invw = oversampling_correction * DIRECT_A3D_ELEM(Fweight, k, i, j);
			DIRECT_A1D_ELEM(sigma2, ires) += invw;
			DIRECT_A1D_ELEM(counter, ires) += 1.;
		}
    }

	// Average (inverse of) sigma2 in reconstruction
	FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(sigma2)
	{
        if (DIRECT_A1D_ELEM(sigma2, i) > 1e-10)
            DIRECT_A1D_ELEM(sigma2, i) = DIRECT_A1D_ELEM(counter, i) / DIRECT_A1D_ELEM(sigma2, i);
        else if (DIRECT_A1D_ELEM(sigma2, i) == 0)
            DIRECT_A1D_ELEM(sigma2, i) = 0.;
		else
		{
			std::cerr << " DIRECT_A1D_ELEM(sigma2, i)= " << DIRECT_A1D_ELEM(sigma2, i) << std::endl;
			REPORT_ERROR("BackProjector::reconstruct: ERROR: unexpectedly small, yet non-zero sigma2 value, this should not happen...a");
        }
    }

	if (update_tau2_with_fsc)
    {
        tau2.reshape(ori_size/2 + 1);
        data_vs_prior.initZeros(ori_size/2 + 1);
		// Then calculate new tau2 values, based on the FSC
		if (!fsc.sameShape(sigma2) || !fsc.sameShape(tau2))
		{
			fsc.printShape(std::cerr);
			tau2.printShape(std::cerr);
			sigma2.printShape(std::cerr);
			REPORT_ERROR("ERROR BackProjector::reconstruct: sigma2, tau2 and fsc have different sizes");
		}
		FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(sigma2)
        {
			// FSC cannot be negative or zero for conversion into tau2
			RFLOAT myfsc = XMIPP_MAX(0.001, DIRECT_A1D_ELEM(fsc, i));
			if (is_whole_instead_of_half)
			{
				// Factor two because of twice as many particles
				// Sqrt-term to get 60-degree phase errors....
				myfsc = sqrt(2. * myfsc / (myfsc + 1.));
			}
			myfsc = XMIPP_MIN(0.999, myfsc);
			RFLOAT myssnr = myfsc / (1. - myfsc);
			// Sjors 29nov2017 try tau2_fudge for pulling harder on Refine3D runs...
            myssnr *= tau2_fudge;
			RFLOAT fsc_based_tau = myssnr * DIRECT_A1D_ELEM(sigma2, i);
			DIRECT_A1D_ELEM(tau2, i) = fsc_based_tau;
			// data_vs_prior is merely for reporting: it is not used for anything in the reconstruction
			DIRECT_A1D_ELEM(data_vs_prior, i) = myssnr;
		}
	}
    RCTOCREC(ReconTimer,ReconS_2);
    RCTICREC(ReconTimer,ReconS_2_5);
	// Apply MAP-additional term to the Fnewweight array
	// This will regularise the actual reconstruction
    if (do_map)
	{

    	// Then, add the inverse of tau2-spectrum values to the weight
		// and also calculate spherical average of data_vs_prior ratios
		if (!update_tau2_with_fsc)
			data_vs_prior.initZeros(ori_size/2 + 1);
		fourier_coverage.initZeros(ori_size/2 + 1);
		counter.initZeros(ori_size/2 + 1);
		FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
 		{
			int r2 = kp * kp + ip * ip + jp * jp;
			if (r2 < max_r2)
			{
				int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
				RFLOAT invw = DIRECT_A3D_ELEM(Fweight, k, i, j);

				RFLOAT invtau2;
				if (DIRECT_A1D_ELEM(tau2, ires) > 0.)
				{
					// Calculate inverse of tau2
					invtau2 = 1. / (oversampling_correction * tau2_fudge * DIRECT_A1D_ELEM(tau2, ires));
				}
				else if (DIRECT_A1D_ELEM(tau2, ires) == 0.)
				{
					// If tau2 is zero, use small value instead
					invtau2 = 1./ ( 0.001 * invw);
				}
				else
				{
					std::cerr << " sigma2= " << sigma2 << std::endl;
					std::cerr << " fsc= " << fsc << std::endl;
					std::cerr << " tau2= " << tau2 << std::endl;
					REPORT_ERROR("ERROR BackProjector::reconstruct: Negative or zero values encountered for tau2 spectrum!");
				}

				// Keep track of spectral evidence-to-prior ratio and remaining noise in the reconstruction
				if (!update_tau2_with_fsc)
					DIRECT_A1D_ELEM(data_vs_prior, ires) += invw / invtau2;

				// Keep track of the coverage in Fourier space
				if (invw / invtau2 >= 1.)
					DIRECT_A1D_ELEM(fourier_coverage, ires) += 1.;

				DIRECT_A1D_ELEM(counter, ires) += 1.;

				// Only for (ires >= minres_map) add Wiener-filter like term
				if (ires >= minres_map)
				{
					// Now add the inverse-of-tau2_class term
					invw += invtau2;
					// Store the new weight again in Fweight
					DIRECT_A3D_ELEM(Fweight, k, i, j) = invw;
				}
			}
		}

		// Average data_vs_prior
		if (!update_tau2_with_fsc)
		{
			FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(data_vs_prior)
			{
				if (i > r_max)
					DIRECT_A1D_ELEM(data_vs_prior, i) = 0.;
				else if (DIRECT_A1D_ELEM(counter, i) < 0.001)
					DIRECT_A1D_ELEM(data_vs_prior, i) = 999.;
				else
					DIRECT_A1D_ELEM(data_vs_prior, i) /= DIRECT_A1D_ELEM(counter, i);
			}
		}

		// Calculate Fourier coverage in each shell
		FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(fourier_coverage)
		{
			if (DIRECT_A1D_ELEM(counter, i) > 0.)
				DIRECT_A1D_ELEM(fourier_coverage, i) /= DIRECT_A1D_ELEM(counter, i);
		}

	} //end if do_map
    else if (do_fsc0999)
    {

     	// Sjors 9may2018: avoid numerical instabilities with unregularised reconstructions....
        FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
        {
            int r2 = kp * kp + ip * ip + jp * jp;
            if (r2 < max_r2)
            {
                int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
                if (ires >= minres_map)
                {
                    // add 1/1000th of the radially averaged sigma2 to the Fweight, to avoid having zeros there...
                	DIRECT_A3D_ELEM(Fweight, k, i, j) += 1./(999. * DIRECT_A1D_ELEM(sigma2, ires));
                }
            }
        }

    }

//==============================================================================add  GPU version

	int Ndim[3];
	int GPU_N=1;
	Ndim[0]=pad_size;
	Ndim[1]=pad_size;
	Ndim[2]=pad_size;
	size_t fullsize= pad_size*pad_size*pad_size;
    initgpu(GPU_N);
	RFLOAT *d_Fweight;
	double *d_Fnewweight;
	int offset;

	MultiGPUplan *dataplan;
	dataplan = (MultiGPUplan *)malloc(sizeof(MultiGPUplan)*1);
	multi_plan_init(dataplan,GPU_N,fullsize,Ndim[0],Ndim[1],Ndim[2]);


	for (int i = 0; i < GPU_N; ++i) {
		cudaSetDevice(dataplan[i].devicenum);
		cudaMalloc((void**) &(dataplan[i].d_Data),sizeof(cufftComplex) * dataplan[i].datasize);
	}

	//=================================
	int xyN[2];
	xyN[0] = Ndim[0];
	xyN[1] = Ndim[1];
	cufftHandle xyplan[1];
	cufftHandle zplan[1];
	int fftoffset[2];
	fftoffset[0] = 0;
	fftoffset[1] = Ndim[0] * (Ndim[1] / 2);
	//===================================


    RCTOCREC(ReconTimer,ReconS_2_5);
	if (skip_gridding)
	{
	    RCTICREC(ReconTimer,ReconS_3);
		std::cerr << "Skipping gridding!" << std::endl;
		Fconv.initZeros(); // to remove any stuff from the input volume
		decenter(data, Fconv, max_r2);

		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			if (DIRECT_MULTIDIM_ELEM(Fweight, n) > 0.)
				DIRECT_MULTIDIM_ELEM(Fconv, n) /= DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		RCTOCREC(ReconTimer,ReconS_3);
#ifdef DEBUG_RECONSTRUCT
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			DIRECT_MULTIDIM_ELEM(ttt(), n) = DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		ttt.write("reconstruct_skipgridding_correction_term.spi");
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			if (DIRECT_MULTIDIM_ELEM(Fweight, n) > 0.)
				DIRECT_MULTIDIM_ELEM(ttt(), n) = 1./DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		ttt.write("reconstruct_skipgridding_correction_term_inverse.spi");
#endif
	}
	else
	{
		RCTICREC(ReconTimer,ReconS_4);
		// Divide both data and Fweight by normalisation factor to prevent FFT's with very large values....
	#ifdef DEBUG_RECONSTRUCT
		std::cerr << " normalise= " << normalise << std::endl;
	#endif
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fweight)
		{
			DIRECT_MULTIDIM_ELEM(Fweight, n) /= normalise;
		}
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(data)
		{
			DIRECT_MULTIDIM_ELEM(data, n) /= normalise;
		}
		RCTOCREC(ReconTimer,ReconS_4);
		RCTICREC(ReconTimer,ReconS_5);
        // Initialise Fnewweight with 1's and 0's. (also see comments below)
		FOR_ALL_ELEMENTS_IN_ARRAY3D(weight)
		{
			if (k * k + i * i + j * j < max_r2)
				A3D_ELEM(weight, k, i, j) = 1.;
			else
				A3D_ELEM(weight, k, i, j) = 0.;
		}



		decenter(weight, Fnewweight, max_r2);
		RCTOCREC(ReconTimer,ReconS_5);
		// Iterative algorithm as in  Eq. [14] in Pipe & Menon (1999)
		// or Eq. (4) in Matej (2001)



		long int Fconvnum=Fconv.nzyxdim;


		long int normsize= Ndim[0]*Ndim[1]*Ndim[2];
		int halfxdim=Fconv.xdim;

		printf("%ld %ld %ld \n",Fconv.xdim,Fconv.ydim,Fconv.zdim);
		printf("Fnewweight: %ld %ld %ld \n",Fnewweight.xdim,Fnewweight.ydim,Fnewweight.zdim);

		int NZ= Ndim[2];
//======for whole divide num along Z ============================================
		int numberZ[4];
		int dividnum=1;
		int i;
		numberZ[0]= pad_size;

//=============================
		dataplan[0].selfZ = numberZ[0]; //+ numberZ[1];


		dataplan[0].realsize=Ndim[0]*Ndim[1]*dataplan[0].selfZ;




		//c_Fconv2 = (cufftComplex *)malloc(fullsize * sizeof(cufftComplex));

#ifdef RELION_SINGLE_PRECISION
		d_Fweight = gpusetdata_float(d_Fweight,Fweight.nzyxdim,Fweight.data);
#else
		d_Fweight = gpusetdata_double(d_Fweight,Fweight.nzyxdim,Fweight.data);
#endif
		d_Fnewweight = gpusetdata_double(d_Fnewweight,Fnewweight.nzyxdim,Fnewweight.data);


         //==================2d fft plan


		cufftPlanMany(&xyplan[0], 2, xyN, NULL, 0, 0, NULL, 0, 0, CUFFT_C2C, pad_size);

	    //==================1d fft plan
		int Ncol[1];
		Ncol[0] = Ndim[1];
		int inembed[3], extraembed[3];
		inembed[0] = Ndim[0];inembed[1] = numberZ[0];inembed[2] = Ndim[2];

		cufftPlanMany(&zplan[0], 1, Ncol, inembed, Ndim[0] * Ndim[1], 1,inembed, Ndim[0] * Ndim[1], 1, CUFFT_C2C, Ndim[0] * (numberZ[0]));




		for (int iter = 0; iter < max_iter_preweight; iter++)
		{

            //std::cout << "    iteration " << (iter+1) << "/" << max_iter_preweight << "\n";
			RCTICREC(ReconTimer,ReconS_6);

			vector_Multi_layout(d_Fnewweight,d_Fweight,dataplan[0].d_Data,fullsize,Fconv.xdim,pad_size);


			cufftExecC2C(xyplan[0], dataplan[0].d_Data,dataplan[0].d_Data, CUFFT_INVERSE);

			cudaDeviceSynchronize();
			cufftExecC2C(zplan[0], dataplan[0].d_Data ,dataplan[0].d_Data, CUFFT_INVERSE);

			cudaDeviceSynchronize();


			RFLOAT normftblob = tab_ftblob(0.);
			RFLOAT *d_tab_ftblob;
			int tabxdim=tab_ftblob.tabulatedValues.xdim;
#ifdef RELION_SINGLE_PRECISION
			d_tab_ftblob=gpusetdata_float(d_tab_ftblob,tab_ftblob.tabulatedValues.xdim,tab_ftblob.tabulatedValues.data);
#else
			d_tab_ftblob=gpusetdata_double(d_tab_ftblob,tab_ftblob.tabulatedValues.xdim,tab_ftblob.tabulatedValues.data);
#endif

			//gpu_kernel2
			volume_Multi_float(dataplan[0].d_Data,d_tab_ftblob, Ndim[0]*Ndim[1]*Ndim[2], tabxdim, tab_ftblob.sampling , pad_size/2, pad_size, ori_size, padding_factor, normftblob);
			cudaDeviceSynchronize();



			cufftExecC2C(xyplan[0], dataplan[0].d_Data ,dataplan[0].d_Data , CUFFT_FORWARD);
			cudaDeviceSynchronize();


			cufftExecC2C(zplan[0], dataplan[0].d_Data ,dataplan[0].d_Data, CUFFT_FORWARD);
			cudaDeviceSynchronize();


			vector_Normlize(dataplan[0].d_Data, normsize ,Ndim[0]*Ndim[1]*Ndim[2]);

			//gpu_kernel3

			fft_Divide(dataplan[0].d_Data,d_Fnewweight,fullsize,pad_size*pad_size,pad_size,pad_size,pad_size,halfxdim,max_r2);

			RCTOCREC(ReconTimer,ReconS_6);
			//cudaMemcpy(c_Fconv,dataplan[0].d_Data,100*sizeof(cufftComplex),cudaMemcpyDeviceToHost);
			//printf("step iter end  from gpu : \n");

	#ifdef DEBUG_RECONSTRUCT
			std::cerr << " PREWEIGHTING ITERATION: "<< iter + 1 << " OF " << max_iter_preweight << std::endl;
			// report of maximum and minimum values of current conv_weight
			std::cerr << " corr_avg= " << corr_avg / corr_nn << std::endl;
			std::cerr << " corr_min= " << corr_min << std::endl;
			std::cerr << " corr_max= " << corr_max << std::endl;
	#endif

		}


		cudaMemcpy(Fnewweight.data,d_Fnewweight,Fnewweight.nzyxdim*sizeof(double),cudaMemcpyDeviceToHost);

		for(int i=0;i<10;i++)
			printf("%f ",Fnewweight.data[i]);
/*
		double sum=0;
		printf("TEMP res :\n");
		for(int i=0;i<Fnewweight.xdim*Fnewweight.ydim*Fnewweight.zdim;i++)
		{
			if(i<10)
				printf("%f ",Fnewweight.data[i]);
			sum+=Fnewweight.data[i];
		}
		printf("sum : %f \n",sum);*/



		for (int i = 0; i < GPU_N; i++) {
			cudaSetDevice(dataplan[i].devicenum);
			cufftDestroy(xyplan[i]);
			cufftDestroy(zplan[i]);
		}
		cudaFree(d_Fnewweight);
		cudaFree(d_Fweight);

		RCTICREC(ReconTimer,ReconS_7);

	#ifdef DEBUG_RECONSTRUCT
		Image<double> tttt;
		tttt()=Fnewweight;
		tttt.write("reconstruct_gridding_weight.spi");
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			DIRECT_MULTIDIM_ELEM(ttt(), n) = abs(DIRECT_MULTIDIM_ELEM(Fconv, n));
		}
		ttt.write("reconstruct_gridding_correction_term.spi");
	#endif


		// Clear memory
		Fweight.clear();

		// Note that Fnewweight now holds the approximation of the inverse of the weights on a regular grid

		// Now do the actual reconstruction with the data array
		// Apply the iteratively determined weight
		Fconv.initZeros(); // to remove any stuff from the input volume
		decenter(data, Fconv, max_r2);
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
#ifdef  RELION_SINGLE_PRECISION
			// Prevent numerical instabilities in single-precision reconstruction with very unevenly sampled orientations
			if (DIRECT_MULTIDIM_ELEM(Fnewweight, n) > 1e20)
				DIRECT_MULTIDIM_ELEM(Fnewweight, n) = 1e20;
#endif
			DIRECT_MULTIDIM_ELEM(Fconv, n) *= DIRECT_MULTIDIM_ELEM(Fnewweight, n);
		}

		// Clear memory
		Fnewweight.clear();
		RCTOCREC(ReconTimer,ReconS_7);
	} // end if skip_gridding

// Gridding theory says one now has to interpolate the fine grid onto the coarse one using a blob kernel
// and then do the inverse transform and divide by the FT of the blob (i.e. do the gridding correction)
// In practice, this gives all types of artefacts (perhaps I never found the right implementation?!)
// Therefore, window the Fourier transform and then do the inverse transform
//#define RECONSTRUCT_CONVOLUTE_BLOB
#ifdef RECONSTRUCT_CONVOLUTE_BLOB

	// Apply the same blob-convolution as above to the data array
	// Mask real-space map beyond its original size to prevent aliasing in the downsampling step below
	RCTICREC(ReconTimer,ReconS_8);
	convoluteBlobRealSpace(transformer, true);
	RCTOCREC(ReconTimer,ReconS_8);
	RCTICREC(ReconTimer,ReconS_9);
	// Now just pick every 3rd pixel in Fourier-space (i.e. down-sample)
	// and do a final inverse FT
	if (ref_dim == 2)
		vol_out.resize(ori_size, ori_size);
	else
		vol_out.resize(ori_size, ori_size, ori_size);
	RCTOCREC(ReconTimer,ReconS_9);
	RCTICREC(ReconTimer,ReconS_10);
	FourierTransformer transformer2;
	MultidimArray<Complex > Ftmp;
	transformer2.setReal(vol_out); // cannot use the first transformer because Fconv is inside there!!
	transformer2.getFourierAlias(Ftmp);
	RCTOCREC(ReconTimer,ReconS_10);
	RCTICREC(ReconTimer,ReconS_11);
	FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Ftmp)
	{
		if (kp * kp + ip * ip + jp * jp < r_max * r_max)
		{
			DIRECT_A3D_ELEM(Ftmp, k, i, j) = FFTW_ELEM(Fconv, kp * padding_factor, ip * padding_factor, jp * padding_factor);
		}
		else
		{
			DIRECT_A3D_ELEM(Ftmp, k, i, j) = 0.;
		}
	}
	RCTOCREC(ReconTimer,ReconS_11);
	RCTICREC(ReconTimer,ReconS_12);
	// inverse FFT leaves result in vol_out
	transformer2.inverseFourierTransform();
	RCTOCREC(ReconTimer,ReconS_12);
	RCTICREC(ReconTimer,ReconS_13);
	// Shift the map back to its origin
	CenterFFT(vol_out, false);
	RCTOCREC(ReconTimer,ReconS_13);
	RCTICREC(ReconTimer,ReconS_14);
	// Un-normalize FFTW (because original FFTs were done with the size of 2D FFTs)
	if (ref_dim==3)
		vol_out /= ori_size;
	RCTOCREC(ReconTimer,ReconS_14);
	RCTICREC(ReconTimer,ReconS_15);
	// Mask out corners to prevent aliasing artefacts
	softMaskOutsideMap(vol_out);
	RCTOCREC(ReconTimer,ReconS_15);
	RCTICREC(ReconTimer,ReconS_16);
	// Gridding correction for the blob
	RFLOAT normftblob = tab_ftblob(0.);
	FOR_ALL_ELEMENTS_IN_ARRAY3D(vol_out)
	{

		RFLOAT r = sqrt((RFLOAT)(k*k+i*i+j*j));
		RFLOAT rval = r / (ori_size * padding_factor);
		A3D_ELEM(vol_out, k, i, j) /= tab_ftblob(rval) / normftblob;
		//if (k==0 && i==0)
		//	std::cerr << " j= " << j << " rval= " << rval << " tab_ftblob(rval) / normftblob= " << tab_ftblob(rval) / normftblob << std::endl;
	}
	RCTOCREC(ReconTimer,ReconS_16);

#else

	// rather than doing the blob-convolution to downsample the data array, do a windowing operation:
	// This is the same as convolution with a SINC. It seems to give better maps.
	// Then just make the blob look as much as a SINC as possible....
	// The "standard" r1.9, m2 and a15 blob looks quite like a sinc until the first zero (perhaps that's why it is standard?)
	//for (RFLOAT r = 0.1; r < 10.; r+=0.01)
	//{
	//	RFLOAT sinc = sin(PI * r / padding_factor ) / ( PI * r / padding_factor);
	//	std::cout << " r= " << r << " sinc= " << sinc << " blob= " << blob_val(r, blob) << std::endl;
	//}

	// Now do inverse FFT and window to original size in real-space
	// Pass the transformer to prevent making and clearing a new one before clearing the one declared above....
	// The latter may give memory problems as detected by electric fence....
	RCTICREC(ReconTimer,ReconS_17);

	// Size of padded real-space volume
	int padoridim = ROUND(padding_factor * ori_size);
	// make sure padoridim is even
	padoridim += padoridim%2;
    vol_out.reshape(padoridim, padoridim, padoridim);
    vol_out.setXmippOrigin();
	fullsize = padoridim *padoridim*padoridim;
	multi_plan_init(dataplan,GPU_N,fullsize,padoridim,padoridim,padoridim);
	if(padoridim > pad_size)
	{
		for (int i = 0; i < GPU_N; ++i) {
			cudaSetDevice(dataplan[i].devicenum);
			cudaFree((dataplan[i].d_Data));
			cudaMalloc((void**) &(dataplan[i].d_Data),sizeof(cufftComplex) * dataplan[i].datasize);
		}

	}

	windowFourierTransform(Fconv, padoridim);

	cufftComplex *c_Fconv;
	c_Fconv = (cufftComplex *)malloc(fullsize * sizeof(cufftComplex));

//	printf("layoutchangecomp : %ld %ld %ld %d\n",Fconv.xdim,Fconv.ydim,Fconv.zdim,padoridim);
	layoutchangecomp(Fconv.data,Fconv.xdim,Fconv.ydim,Fconv.zdim,padoridim,c_Fconv);

//init plan and for even data



	dataplan[0].selfZ=padoridim;
	int numberZ[1];
	numberZ[0]= dataplan[0].selfZ;

	xyN[0] = padoridim;
	xyN[1] = padoridim;
    //==================2d fft plan


	cufftPlanMany(&xyplan[0], 2, xyN, NULL, 0, 0, NULL, 0, 0, CUFFT_C2C, padoridim);

   //==================1d fft plan
	int Ncol[1];
	Ncol[0] = padoridim;
	int inembed[3];
	inembed[0] = padoridim;inembed[1] = dataplan[0].selfZ;inembed[2] = padoridim;
	cufftPlanMany(&zplan[0], 1, Ncol, inembed, padoridim * padoridim, 1,inembed, padoridim * padoridim, 1, CUFFT_C2C, padoridim * (dataplan[0].selfZ));


	cudaMemcpy(dataplan[0].d_Data,c_Fconv,sizeof(cufftComplex)*fullsize,cudaMemcpyHostToDevice);

	//multi_memcpy_data(dataplan,c_Fconv2,GPU_N,padoridim,padoridim);

	cufftExecC2C(xyplan[0], dataplan[0].d_Data ,dataplan[0].d_Data , CUFFT_INVERSE);


	cufftExecC2C(zplan[0], dataplan[0].d_Data ,dataplan[0].d_Data, CUFFT_INVERSE);

	//multi_memcpy_databack(dataplan,c_Fconv2,GPU_N,padoridim,padoridim);
	cudaMemcpy(c_Fconv,dataplan[0].d_Data,sizeof(cufftComplex)*fullsize,cudaMemcpyDeviceToHost);
	cufftDestroy(zplan[0]);
	cufftDestroy(xyplan[0]);
	cudaFree(dataplan[0].d_Data);
	cudaDeviceSynchronize();

	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(dataplan[i].devicenum);
		size_t freedata1,total1;
		cudaMemGetInfo( &freedata1, &total1 );
		printf("After alloccation  : %ld   %ld and gpu num %d \n",freedata1,total1,i);
	}



//	cudaMemcpy(dataplan[0].d_Data,c_Fconv,padoridim *padoridim*padoridim *sizeof(cufftComplex),cudaMemcpyHostToDevice);
//	cufftPlan3d( &cuffthandle2, padoridim, padoridim, padoridim , CUFFT_C2C);
//	cufftExecC2C(cuffthandle2, dataplan[0].d_Data, dataplan[0].d_Data, CUFFT_INVERSE);
	//cudaMemcpy(c_Fconv,dataplan[0].d_Data,padoridim *padoridim*padoridim *sizeof(cufftComplex),cudaMemcpyDeviceToHost);
    for(int i=0;i<padoridim *padoridim*padoridim;i++)
    	vol_out.data[i]=c_Fconv[i].x;

    printf("FINAL:");
    for(int i=0;i<10;i++)
    	printf("%f ",vol_out.data[i]);

    free(c_Fconv);
/*
 *  method 1
	cufftReal *d_Freal;
	cudaMalloc((void**) &d_Freal, sizeof(cufftReal)*padoridim*padoridim*padoridim);
	printf("Before %d \n",Fconv.nzyxdim);
	windowFourierTransform(Fconv, padoridim);
	printf("After %d \n",Fconv.nzyxdim);
	cudaMemcpy(d_Fconv,Fconv.data,Fconv.nzyxdim *sizeof(cufftComplex),cudaMemcpyHostToDevice);
	cufftPlan3d( &cufftHandle2, padoridim, padoridim, padoridim , CUFFT_C2R);
	cufftExecC2R(cufftHandle2, d_Fconv, d_Freal);
	vol_out.reshape(padoridim, padoridim, padoridim);
	vol_out.setXmippOrigin();
	cudaMemcpy(vol_out.data,d_Freal,vol_out.nzyxdim *sizeof(cufftReal),cudaMemcpyDeviceToHost);
	*/


    CenterFFT(vol_out,true);

	// Window in real-space
	if (ref_dim==2)
	{
		vol_out.window(FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size),
				       LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size));
	}
	else
	{
		vol_out.window(FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size),
				       LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size));
	}
	vol_out.setXmippOrigin();

	// Normalisation factor of FFTW
	// The Fourier Transforms are all "normalised" for 2D transforms of size = ori_size x ori_size
	float normfft = (RFLOAT)(padding_factor * padding_factor * padding_factor * ori_size);
	vol_out /= normfft;
	// Mask out corners to prevent aliasing artefacts
	softMaskOutsideMap(vol_out);
	//windowToOridimRealSpace(transformer, vol_out, nr_threads, printTimes);
	RCTOCREC(ReconTimer,ReconS_17);


#endif

#ifdef DEBUG_RECONSTRUCT
	ttt()=vol_out;
	ttt.write("reconstruct_before_gridding_correction.spi");
#endif

	// Correct for the linear/nearest-neighbour interpolation that led to the data array
	RCTICREC(ReconTimer,ReconS_18);
	griddingCorrect(vol_out);
	RCTOCREC(ReconTimer,ReconS_18);
	// If the tau-values were calculated based on the FSC, then now re-calculate the power spectrum of the actual reconstruction
	if (update_tau2_with_fsc)
	{

		// New tau2 will be the power spectrum of the new map
		MultidimArray<RFLOAT> spectrum, count;

		// Calculate this map's power spectrum
		// Don't call getSpectrum() because we want to use the same transformer object to prevent memory trouble....
		RCTICREC(ReconTimer,ReconS_19);
		spectrum.initZeros(XSIZE(vol_out));
	    count.initZeros(XSIZE(vol_out));
		RCTOCREC(ReconTimer,ReconS_19);
		RCTICREC(ReconTimer,ReconS_20);
	    // recycle the same transformer for all images
        transformer.setReal(vol_out);
		RCTOCREC(ReconTimer,ReconS_20);
		RCTICREC(ReconTimer,ReconS_21);
        transformer.FourierTransform();
		RCTOCREC(ReconTimer,ReconS_21);
		RCTICREC(ReconTimer,ReconS_22);
	    FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
	    {
	    	long int idx = ROUND(sqrt(kp*kp + ip*ip + jp*jp));
	    	spectrum(idx) += norm(dAkij(Fconv, k, i, j));
	        count(idx) += 1.;
	    }
	    spectrum /= count;

		// Factor two because of two-dimensionality of the complex plane
		// (just like sigma2_noise estimates, the power spectra should be divided by 2)
		RFLOAT normfft = (ref_dim == 3 && data_dim == 2) ? (RFLOAT)(ori_size * ori_size) : 1.;
		spectrum *= normfft / 2.;

		// New SNR^MAP will be power spectrum divided by the noise in the reconstruction (i.e. sigma2)
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(data_vs_prior)
		{
			DIRECT_MULTIDIM_ELEM(tau2, n) =  tau2_fudge * DIRECT_MULTIDIM_ELEM(spectrum, n);
		}
		RCTOCREC(ReconTimer,ReconS_22);
	}
	RCTICREC(ReconTimer,ReconS_23);
	// Completely empty the transformer object
	transformer.cleanup();
    // Now can use extra mem to move data into smaller array space
    vol_out.shrinkToFit();

	RCTOCREC(ReconTimer,ReconS_23);
#ifdef TIMINGREC
    if(printTimes)
    	ReconTimer.printTimes(true);
#endif


#ifdef DEBUG_RECONSTRUCT
    std::cerr<<"done with reconstruct"<<std::endl;
#endif

	tau2_io = tau2;
    sigma2_out = sigma2;
    data_vs_prior_out = data_vs_prior;
    fourier_coverage_out = fourier_coverage;
}


/*
void BackProjector::reconstruct_gpu_transpose(MultidimArray<RFLOAT> &vol_out,
                                int max_iter_preweight,
                                bool do_map,
                                RFLOAT tau2_fudge,
                                MultidimArray<RFLOAT> &tau2_io, // can be input/output
                                MultidimArray<RFLOAT> &sigma2_out,
                                MultidimArray<RFLOAT> &data_vs_prior_out,
                                MultidimArray<RFLOAT> &fourier_coverage_out,
                                const MultidimArray<RFLOAT> &fsc, // only input
                                RFLOAT normalise,
                                bool update_tau2_with_fsc,
                                bool is_whole_instead_of_half,
                                int nr_threads,
                                int minres_map,
                                bool printTimes,
								bool do_fsc0999)
{

#ifdef TIMINGREC
	Timer ReconTimer;
	printTimes=1;
	int ReconS_1 = ReconTimer.setNew(" RcS1_Init ");
	int ReconS_2 = ReconTimer.setNew(" RcS2_Shape&Noise ");
	int ReconS_2_5 = ReconTimer.setNew(" RcS2.5_Regularize ");
	int ReconS_3 = ReconTimer.setNew(" RcS3_skipGridding ");
	int ReconS_4 = ReconTimer.setNew(" RcS4_doGridding_norm ");
	int ReconS_5 = ReconTimer.setNew(" RcS5_doGridding_init ");
	int ReconS_6 = ReconTimer.setNew(" RcS6_doGridding_iter ");
	int ReconS_7 = ReconTimer.setNew(" RcS7_doGridding_apply ");
	int ReconS_8 = ReconTimer.setNew(" RcS8_blobConvolute ");
	int ReconS_9 = ReconTimer.setNew(" RcS9_blobResize ");
	int ReconS_10 = ReconTimer.setNew(" RcS10_blobSetReal ");
	int ReconS_11 = ReconTimer.setNew(" RcS11_blobSetTemp ");
	int ReconS_12 = ReconTimer.setNew(" RcS12_blobTransform ");
	int ReconS_13 = ReconTimer.setNew(" RcS13_blobCenterFFT ");
	int ReconS_14 = ReconTimer.setNew(" RcS14_blobNorm1 ");
	int ReconS_15 = ReconTimer.setNew(" RcS15_blobSoftMask ");
	int ReconS_16 = ReconTimer.setNew(" RcS16_blobNorm2 ");
	int ReconS_17 = ReconTimer.setNew(" RcS17_WindowReal ");
	int ReconS_18 = ReconTimer.setNew(" RcS18_GriddingCorrect ");
	int ReconS_19 = ReconTimer.setNew(" RcS19_tauInit ");
	int ReconS_20 = ReconTimer.setNew(" RcS20_tausetReal ");
	int ReconS_21 = ReconTimer.setNew(" RcS21_tauTransform ");
	int ReconS_22 = ReconTimer.setNew(" RcS22_tautauRest ");
	int ReconS_23 = ReconTimer.setNew(" RcS23_tauShrinkToFit ");
	int ReconS_24 = ReconTimer.setNew(" RcS24_extra ");
#endif

    // never rely on references (handed to you from the outside) for computation:
    // they could be the same (i.e. reconstruct(..., dummy, dummy, dummy, dummy, ...); )

    MultidimArray<RFLOAT> sigma2, data_vs_prior, fourier_coverage;
	MultidimArray<RFLOAT> tau2 = tau2_io;


    RCTICREC(ReconTimer,ReconS_1);
    FourierTransformer transformer;
	MultidimArray<RFLOAT> Fweight;
	// Fnewweight can become too large for a float: always keep this one in double-precision
	MultidimArray<double> Fnewweight;
	MultidimArray<Complex>& Fconv = transformer.getFourierReference();
	int max_r2 = ROUND(r_max * padding_factor) * ROUND(r_max * padding_factor);

//#define DEBUG_RECONSTRUCT
#ifdef DEBUG_RECONSTRUCT
	Image<RFLOAT> ttt;
	FileName fnttt;
	ttt()=weight;
	ttt.write("reconstruct_initial_weight.spi");
	std::cerr << " pad_size= " << pad_size << " padding_factor= " << padding_factor << " max_r2= " << max_r2 << std::endl;
#endif

    // Set Fweight, Fnewweight and Fconv to the right size
    if (ref_dim == 2)
        vol_out.setDimensions(pad_size, pad_size, 1, 1);
    else
        // Too costly to actually allocate the space
        // Trick transformer with the right dimensions
        vol_out.setDimensions(pad_size, pad_size, pad_size, 1);

    transformer.setReal(vol_out); // Fake set real. 1. Allocate space for Fconv 2. calculate plans.
    vol_out.clear(); // Reset dimensions to 0

    RCTOCREC(ReconTimer,ReconS_1);
    RCTICREC(ReconTimer,ReconS_2);

    Fweight.reshape(Fconv);
    if (!skip_gridding)
    	Fnewweight.reshape(Fconv);

	// Go from projector-centered to FFTW-uncentered
	decenter(weight, Fweight, max_r2);

	// Take oversampling into account
	RFLOAT oversampling_correction = (ref_dim == 3) ? (padding_factor * padding_factor * padding_factor) : (padding_factor * padding_factor);
	MultidimArray<RFLOAT> counter;

	// First calculate the radial average of the (inverse of the) power of the noise in the reconstruction
	// This is the left-hand side term in the nominator of the Wiener-filter-like update formula
	// and it is stored inside the weight vector
	// Then, if (do_map) add the inverse of tau2-spectrum values to the weight
	sigma2.initZeros(ori_size/2 + 1);
	counter.initZeros(ori_size/2 + 1);
	FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
	{
		int r2 = kp * kp + ip * ip + jp * jp;
		if (r2 < max_r2)
		{
			int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
			RFLOAT invw = oversampling_correction * DIRECT_A3D_ELEM(Fweight, k, i, j);
			DIRECT_A1D_ELEM(sigma2, ires) += invw;
			DIRECT_A1D_ELEM(counter, ires) += 1.;
		}
    }

	// Average (inverse of) sigma2 in reconstruction
	FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(sigma2)
	{
        if (DIRECT_A1D_ELEM(sigma2, i) > 1e-10)
            DIRECT_A1D_ELEM(sigma2, i) = DIRECT_A1D_ELEM(counter, i) / DIRECT_A1D_ELEM(sigma2, i);
        else if (DIRECT_A1D_ELEM(sigma2, i) == 0)
            DIRECT_A1D_ELEM(sigma2, i) = 0.;
		else
		{
			std::cerr << " DIRECT_A1D_ELEM(sigma2, i)= " << DIRECT_A1D_ELEM(sigma2, i) << std::endl;
			REPORT_ERROR("BackProjector::reconstruct: ERROR: unexpectedly small, yet non-zero sigma2 value, this should not happen...a");
        }
    }

	if (update_tau2_with_fsc)
    {
        tau2.reshape(ori_size/2 + 1);
        data_vs_prior.initZeros(ori_size/2 + 1);
		// Then calculate new tau2 values, based on the FSC
		if (!fsc.sameShape(sigma2) || !fsc.sameShape(tau2))
		{
			fsc.printShape(std::cerr);
			tau2.printShape(std::cerr);
			sigma2.printShape(std::cerr);
			REPORT_ERROR("ERROR BackProjector::reconstruct: sigma2, tau2 and fsc have different sizes");
		}
		FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(sigma2)
        {
			// FSC cannot be negative or zero for conversion into tau2
			RFLOAT myfsc = XMIPP_MAX(0.001, DIRECT_A1D_ELEM(fsc, i));
			if (is_whole_instead_of_half)
			{
				// Factor two because of twice as many particles
				// Sqrt-term to get 60-degree phase errors....
				myfsc = sqrt(2. * myfsc / (myfsc + 1.));
			}
			myfsc = XMIPP_MIN(0.999, myfsc);
			RFLOAT myssnr = myfsc / (1. - myfsc);
			// Sjors 29nov2017 try tau2_fudge for pulling harder on Refine3D runs...
            myssnr *= tau2_fudge;
			RFLOAT fsc_based_tau = myssnr * DIRECT_A1D_ELEM(sigma2, i);
			DIRECT_A1D_ELEM(tau2, i) = fsc_based_tau;
			// data_vs_prior is merely for reporting: it is not used for anything in the reconstruction
			DIRECT_A1D_ELEM(data_vs_prior, i) = myssnr;
		}
	}
    RCTOCREC(ReconTimer,ReconS_2);
    RCTICREC(ReconTimer,ReconS_2_5);
	// Apply MAP-additional term to the Fnewweight array
	// This will regularise the actual reconstruction
    if (do_map)
	{

    	// Then, add the inverse of tau2-spectrum values to the weight
		// and also calculate spherical average of data_vs_prior ratios
		if (!update_tau2_with_fsc)
			data_vs_prior.initZeros(ori_size/2 + 1);
		fourier_coverage.initZeros(ori_size/2 + 1);
		counter.initZeros(ori_size/2 + 1);
		FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
 		{
			int r2 = kp * kp + ip * ip + jp * jp;
			if (r2 < max_r2)
			{
				int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
				RFLOAT invw = DIRECT_A3D_ELEM(Fweight, k, i, j);

				RFLOAT invtau2;
				if (DIRECT_A1D_ELEM(tau2, ires) > 0.)
				{
					// Calculate inverse of tau2
					invtau2 = 1. / (oversampling_correction * tau2_fudge * DIRECT_A1D_ELEM(tau2, ires));
				}
				else if (DIRECT_A1D_ELEM(tau2, ires) == 0.)
				{
					// If tau2 is zero, use small value instead
					invtau2 = 1./ ( 0.001 * invw);
				}
				else
				{
					std::cerr << " sigma2= " << sigma2 << std::endl;
					std::cerr << " fsc= " << fsc << std::endl;
					std::cerr << " tau2= " << tau2 << std::endl;
					REPORT_ERROR("ERROR BackProjector::reconstruct: Negative or zero values encountered for tau2 spectrum!");
				}

				// Keep track of spectral evidence-to-prior ratio and remaining noise in the reconstruction
				if (!update_tau2_with_fsc)
					DIRECT_A1D_ELEM(data_vs_prior, ires) += invw / invtau2;

				// Keep track of the coverage in Fourier space
				if (invw / invtau2 >= 1.)
					DIRECT_A1D_ELEM(fourier_coverage, ires) += 1.;

				DIRECT_A1D_ELEM(counter, ires) += 1.;

				// Only for (ires >= minres_map) add Wiener-filter like term
				if (ires >= minres_map)
				{
					// Now add the inverse-of-tau2_class term
					invw += invtau2;
					// Store the new weight again in Fweight
					DIRECT_A3D_ELEM(Fweight, k, i, j) = invw;
				}
			}
		}

		// Average data_vs_prior
		if (!update_tau2_with_fsc)
		{
			FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(data_vs_prior)
			{
				if (i > r_max)
					DIRECT_A1D_ELEM(data_vs_prior, i) = 0.;
				else if (DIRECT_A1D_ELEM(counter, i) < 0.001)
					DIRECT_A1D_ELEM(data_vs_prior, i) = 999.;
				else
					DIRECT_A1D_ELEM(data_vs_prior, i) /= DIRECT_A1D_ELEM(counter, i);
			}
		}

		// Calculate Fourier coverage in each shell
		FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(fourier_coverage)
		{
			if (DIRECT_A1D_ELEM(counter, i) > 0.)
				DIRECT_A1D_ELEM(fourier_coverage, i) /= DIRECT_A1D_ELEM(counter, i);
		}

	} //end if do_map
    else if (do_fsc0999)
    {

     	// Sjors 9may2018: avoid numerical instabilities with unregularised reconstructions....
        FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
        {
            int r2 = kp * kp + ip * ip + jp * jp;
            if (r2 < max_r2)
            {
                int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
                if (ires >= minres_map)
                {
                    // add 1/1000th of the radially averaged sigma2 to the Fweight, to avoid having zeros there...
                	DIRECT_A3D_ELEM(Fweight, k, i, j) += 1./(999. * DIRECT_A1D_ELEM(sigma2, ires));
                }
            }
        }

    }

//==============================================================================add  GPU version

	int Ndim[3];
	int devCount;
	cudaGetDeviceCount(&devCount);
	int GPU_N=devCount;

	Ndim[0]=pad_size;
	Ndim[1]=pad_size;
	Ndim[2]=pad_size;
	size_t fullsize= pad_size*pad_size*pad_size;
    initgpu(GPU_N);

	int offset;
	//divide
	int *numberZ = (int *)malloc(sizeof(int)*GPU_N);
	int *offsetZ = (int *)malloc(sizeof(int)*GPU_N);
	dividetask(numberZ,offsetZ,pad_size,GPU_N);

	MultiGPUplan *plan;
	plan = (MultiGPUplan *)malloc(sizeof(MultiGPUplan)*GPU_N);
	multi_plan_init_transpose(plan,GPU_N,numberZ,offsetZ,pad_size);



	for (int i = 0; i < GPU_N; ++i) {
		cudaSetDevice(plan[i].devicenum);
		cudaMalloc((void**) & (plan[i].d_Data),sizeof(cufftComplex) * plan[i].datasize);
		cudaMalloc((void**) & (plan[i].temp_Data), sizeof(cufftComplex) * plan[i].tempsize);
	}

	//=================================
	int xyN[2];
	xyN[0] = Ndim[0];
	xyN[1] = Ndim[1];
	cufftHandle *xyplan;
	xyplan = (cufftHandle*) malloc(sizeof(cufftHandle)*GPU_N);
	cufftHandle *zplan;
	zplan = (cufftHandle*) malloc(sizeof(cufftHandle)*GPU_N);


    RCTOCREC(ReconTimer,ReconS_2_5);
	if (skip_gridding)
	{
	    RCTICREC(ReconTimer,ReconS_3);
		std::cerr << "Skipping gridding!" << std::endl;
		Fconv.initZeros(); // to remove any stuff from the input volume
		decenter(data, Fconv, max_r2);

		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			if (DIRECT_MULTIDIM_ELEM(Fweight, n) > 0.)
				DIRECT_MULTIDIM_ELEM(Fconv, n) /= DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		RCTOCREC(ReconTimer,ReconS_3);
#ifdef DEBUG_RECONSTRUCT
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			DIRECT_MULTIDIM_ELEM(ttt(), n) = DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		ttt.write("reconstruct_skipgridding_correction_term.spi");
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			if (DIRECT_MULTIDIM_ELEM(Fweight, n) > 0.)
				DIRECT_MULTIDIM_ELEM(ttt(), n) = 1./DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		ttt.write("reconstruct_skipgridding_correction_term_inverse.spi");
#endif
	}
	else
	{
		RCTICREC(ReconTimer,ReconS_4);
		// Divide both data and Fweight by normalisation factor to prevent FFT's with very large values....
	#ifdef DEBUG_RECONSTRUCT
		std::cerr << " normalise= " << normalise << std::endl;
	#endif
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fweight)
		{
			DIRECT_MULTIDIM_ELEM(Fweight, n) /= normalise;
		}
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(data)
		{
			DIRECT_MULTIDIM_ELEM(data, n) /= normalise;
		}
		RCTOCREC(ReconTimer,ReconS_4);
		RCTICREC(ReconTimer,ReconS_5);
        // Initialise Fnewweight with 1's and 0's. (also see comments below)
		FOR_ALL_ELEMENTS_IN_ARRAY3D(weight)
		{
			if (k * k + i * i + j * j < max_r2)
				A3D_ELEM(weight, k, i, j) = 1.;
			else
				A3D_ELEM(weight, k, i, j) = 0.;
		}



		decenter(weight, Fnewweight, max_r2);
		RCTOCREC(ReconTimer,ReconS_5);
		// Iterative algorithm as in  Eq. [14] in Pipe & Menon (1999)
		// or Eq. (4) in Matej (2001)


		long int Fconvnum=Fconv.nzyxdim;
		long int normsize= Ndim[0]*Ndim[1]*Ndim[2];
		int halfxdim=Fconv.xdim;

		printf("%ld %ld %ld \n",Fconv.xdim,Fconv.ydim,Fconv.zdim);
		printf("Fnewweight: %ld %ld %ld \n",Fnewweight.xdim,Fnewweight.ydim,Fnewweight.zdim);
		multi_enable_access(plan,GPU_N);


         //==================2d fft plan

		for (int i = 0; i < GPU_N; i++) {
		    cudaSetDevice(plan[i].devicenum);
			cufftPlanMany(&xyplan[i], 2, xyN, NULL, 0, 0, NULL, 0, 0, CUFFT_C2C, numberZ[i]);
		}
	    //==================1d fft plan

		for (int i = 0; i < GPU_N; i++) {
		    cudaSetDevice(plan[i].devicenum);
			int rank=1;
			int nrank[1];
			nrank[0]=Ndim[1];
			int inembed[2];
			inembed[0]=Ndim[0];
			inembed[1]=Ndim[1];
			cufftPlanMany(&zplan[i], rank, nrank, inembed, Ndim[0] , 1, inembed, Ndim[0], 1, CUFFT_C2C, Ndim[0]);
		}


		float *fweightdata= (float *)malloc(sizeof(float)*pad_size*pad_size*pad_size);
		layoutchange(Fweight.data,Fweight.xdim,Fweight.ydim,Fweight.zdim,pad_size,fweightdata);
		float **d_blocktwo;
		d_blocktwo =(float **)malloc(GPU_N*sizeof(float*));
		for(int i=0;i< GPU_N;i++)
		{
			cudaSetDevice(plan[i].devicenum);
			cudaMalloc((void**) &(d_blocktwo[i]),sizeof(float) * plan[i].realsize);
			cudaMemcpy(d_blocktwo[i],fweightdata+plan[i].selfoffset,plan[i].realsize*sizeof(float),cudaMemcpyHostToDevice);
		}
		multi_sync(plan,GPU_N);
		free(fweightdata);


		double *tempdata= (double *)malloc(sizeof(double)*pad_size*pad_size*pad_size);
		// every thread map two block to used
		layoutchange(Fnewweight.data,Fnewweight.xdim,Fnewweight.ydim,Fnewweight.zdim,pad_size,tempdata);
		double **d_blockone;
		d_blockone = (double **)malloc(GPU_N*sizeof(double *));

		for (int i = 0; i < GPU_N; i++) {
		    cudaSetDevice(plan[i].devicenum);
			cudaMalloc((void**) &(d_blockone[i]),sizeof(double) * plan[i].realsize);
			cudaMemcpy(d_blockone[i],tempdata+plan[i].selfoffset,plan[i].realsize*sizeof(double),cudaMemcpyHostToDevice);
		}
		multi_sync(plan,GPU_N);



		cufftComplex *cpu_data=(cufftComplex *)malloc(sizeof(cufftComplex)*100);

		for (int iter = 0; iter < max_iter_preweight; iter++)
		{

            //std::cout << "    iteration " << (iter+1) << "/" << max_iter_preweight << "\n";
			RCTICREC(ReconTimer,ReconS_6);

			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(plan[i].devicenum);
				vector_Multi_layout_mpi(d_blockone[i],d_blocktwo[i],plan[i].d_Data, plan[i].realsize);
				cufftExecC2C(xyplan[i], plan[i].d_Data,plan[i].d_Data , CUFFT_INVERSE);
				cudaDeviceSynchronize();
			}
			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(plan[i].devicenum);
				cudaMemcpy(cpu_data,plan[i].d_Data,pad_size*sizeof(cufftComplex),cudaMemcpyDeviceToHost);
				for(int i=0;i<10;i++)
					printf("%f ",cpu_data[i].x);
			}

			printf("\n");

			yzlocal_transpose(plan,GPU_N,pad_size,offsetZ);
			transpose_exchange(plan,GPU_N,pad_size,offsetZ);
			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(plan[i].devicenum);
				int offset = 0;
				for(int zslice=0;zslice<numberZ[i];zslice++)
				{
					cufftExecC2C(zplan[i], plan[i].d_Data+offset, plan[i].d_Data+offset, CUFFT_INVERSE);
					offset+= pad_size*pad_size;
				}
				cudaDeviceSynchronize();
			}
			//transpose again
			//yzlocal_transpose(plan,GPU_N,pad_size,offsetZ);
			//transpose_exchange(plan,GPU_N,pad_size,offsetZ);

			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(plan[i].devicenum);
				cudaMemcpy(cpu_data,plan[i].d_Data,pad_size*sizeof(cufftComplex),cudaMemcpyDeviceToHost);
				for(int i=0;i<10;i++)
					printf("%f ",cpu_data[i].x);
			}


			RFLOAT normftblob = tab_ftblob(0.);
			RFLOAT *d_tab_ftblob;
			int tabxdim=tab_ftblob.tabulatedValues.xdim;
#ifdef RELION_SINGLE_PRECISION
			d_tab_ftblob=gpusetdata_float(d_tab_ftblob,tab_ftblob.tabulatedValues.xdim,tab_ftblob.tabulatedValues.data);
			printf("\nnorm bolb before \n");
#else
			d_tab_ftblob=gpusetdata_double(d_tab_ftblob,tab_ftblob.tabulatedValues.xdim,tab_ftblob.tabulatedValues.data);
#endif
			//gpu_kernel2
			for(int i = 0; i < GPU_N; i++)
			{
				cudaSetDevice(plan[i].devicenum);
				volume_Multi_float_transone(plan[i].d_Data,d_tab_ftblob, Ndim[0]*Ndim[1]*numberZ[i],
									tabxdim, tab_ftblob.sampling , pad_size/2, pad_size, ori_size, padding_factor, normftblob,
									Ndim[1],offsetZ[i]);
				cudaDeviceSynchronize();
			}
			printf("\nbefore 2d fft \n");
			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(plan[i].devicenum);
				cudaMemcpy(cpu_data,plan[i].d_Data,pad_size*sizeof(cufftComplex),cudaMemcpyDeviceToHost);
				for(int i=0;i<10;i++)
					printf("%f ",cpu_data[i].x);
			}


			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(plan[i].devicenum);
				cufftExecC2C(xyplan[i], plan[i].d_Data,plan[i].d_Data , CUFFT_FORWARD);
				cudaDeviceSynchronize();
			}
			printf("\n2d fft after before send  \n");
			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(plan[i].devicenum);
				cudaMemcpy(cpu_data,plan[i].d_Data,pad_size*sizeof(cufftComplex),cudaMemcpyDeviceToHost);
				for(int i=0;i<10;i++)
					printf("%f ",cpu_data[i].x);
			}
			yzlocal_transpose(plan,GPU_N,pad_size,offsetZ);
			transpose_exchange(plan,GPU_N,pad_size,offsetZ);
			printf("\n2d fft after after send  \n");
			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(plan[i].devicenum);
				cudaMemcpy(cpu_data,plan[i].d_Data,pad_size*sizeof(cufftComplex),cudaMemcpyDeviceToHost);
				for(int i=0;i<10;i++)
					printf("%f ",cpu_data[i].x);
			}

			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(plan[i].devicenum);
				int offset = 0;
				for(int zslice=0;zslice<numberZ[i];zslice++)
				{
					cufftExecC2C(zplan[i], plan[i].d_Data+offset, plan[i].d_Data+offset, CUFFT_FORWARD);
					offset+= pad_size*pad_size;
				}
				cudaDeviceSynchronize();
			}

			//transpose again
			printf("\n 1d fft after \n");
			yzlocal_transpose(plan,GPU_N,pad_size,offsetZ);
			transpose_exchange(plan,GPU_N,pad_size,offsetZ);

			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(plan[i].devicenum);
				vector_Normlize(plan[i].d_Data, normsize ,Ndim[0]*Ndim[1]*numberZ[i]);
				//gpu_kernel3
				fft_Divide_mpi(plan[i].d_Data,d_blockone[i],plan[i].realsize,pad_size*pad_size,pad_size,pad_size,pad_size,Fconv.xdim,max_r2,offsetZ[i]);
			}

			RCTOCREC(ReconTimer,ReconS_6);
			//cudaMemcpy(c_Fconv,plan[0].d_Data,100*sizeof(cufftComplex),cudaMemcpyDeviceToHost);
			//printf("step iter end  from gpu : \n");

	#ifdef DEBUG_RECONSTRUCT
			std::cerr << " PREWEIGHTING ITERATION: "<< iter + 1 << " OF " << max_iter_preweight << std::endl;
			// report of maximum and minimum values of current conv_weight
			std::cerr << " corr_avg= " << corr_avg / corr_nn << std::endl;
			std::cerr << " corr_min= " << corr_min << std::endl;
			std::cerr << " corr_max= " << corr_max << std::endl;
	#endif

		}

		for(int i=0;i<GPU_N; i++){
			cudaSetDevice(plan[i].devicenum);
			cudaMemcpy(tempdata+plan[i].selfoffset,d_blockone[i],plan[i].realsize*sizeof(double),cudaMemcpyDeviceToHost);
		}
		layoutchangeback(tempdata,Fnewweight.xdim,Fnewweight.ydim,Fnewweight.zdim,pad_size,Fnewweight.data);


		printf("TEMP res :\n");
		for(int i=0;i<10;i++)
			printf("%f ",Fnewweight.data[i]);


		for (int i = 0; i < GPU_N; i++) {
			cudaSetDevice(plan[i].devicenum);
			cufftDestroy(xyplan[i]);
			cufftDestroy(zplan[i]);
			cudaFree(d_blockone[i]);
			cudaFree(d_blocktwo[i]);
		}
		free(tempdata);
		free(d_blockone);
		free(d_blocktwo);
		RCTICREC(ReconTimer,ReconS_7);

		for (int i = 0; i < GPU_N; i++) {
			cudaSetDevice(plan[i].devicenum);
			size_t freedata1,total1;
			cudaMemGetInfo( &freedata1, &total1 );
			printf("After alloccation loop : %ld   %ld and gpu num %d \n",freedata1,total1,i);
		}

	#ifdef DEBUG_RECONSTRUCT
		Image<double> tttt;
		tttt()=Fnewweight;
		tttt.write("reconstruct_gridding_weight.spi");
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			DIRECT_MULTIDIM_ELEM(ttt(), n) = abs(DIRECT_MULTIDIM_ELEM(Fconv, n));
		}
		ttt.write("reconstruct_gridding_correction_term.spi");
	#endif


		// Clear memory
		Fweight.clear();

		// Note that Fnewweight now holds the approximation of the inverse of the weights on a regular grid

		// Now do the actual reconstruction with the data array
		// Apply the iteratively determined weight
		Fconv.initZeros(); // to remove any stuff from the input volume
		decenter(data, Fconv, max_r2);
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
#ifdef  RELION_SINGLE_PRECISION
			// Prevent numerical instabilities in single-precision reconstruction with very unevenly sampled orientations
			if (DIRECT_MULTIDIM_ELEM(Fnewweight, n) > 1e20)
				DIRECT_MULTIDIM_ELEM(Fnewweight, n) = 1e20;
#endif
			DIRECT_MULTIDIM_ELEM(Fconv, n) *= DIRECT_MULTIDIM_ELEM(Fnewweight, n);
		}

		// Clear memory
		Fnewweight.clear();
		RCTOCREC(ReconTimer,ReconS_7);
	} // end if skip_gridding

// Gridding theory says one now has to interpolate the fine grid onto the coarse one using a blob kernel
// and then do the inverse transform and divide by the FT of the blob (i.e. do the gridding correction)
// In practice, this gives all types of artefacts (perhaps I never found the right implementation?!)
// Therefore, window the Fourier transform and then do the inverse transform
//#define RECONSTRUCT_CONVOLUTE_BLOB
#ifdef RECONSTRUCT_CONVOLUTE_BLOB

	// Apply the same blob-convolution as above to the data array
	// Mask real-space map beyond its original size to prevent aliasing in the downsampling step below
	RCTICREC(ReconTimer,ReconS_8);
	convoluteBlobRealSpace(transformer, true);
	RCTOCREC(ReconTimer,ReconS_8);
	RCTICREC(ReconTimer,ReconS_9);
	// Now just pick every 3rd pixel in Fourier-space (i.e. down-sample)
	// and do a final inverse FT
	if (ref_dim == 2)
		vol_out.resize(ori_size, ori_size);
	else
		vol_out.resize(ori_size, ori_size, ori_size);
	RCTOCREC(ReconTimer,ReconS_9);
	RCTICREC(ReconTimer,ReconS_10);
	FourierTransformer transformer2;
	MultidimArray<Complex > Ftmp;
	transformer2.setReal(vol_out); // cannot use the first transformer because Fconv is inside there!!
	transformer2.getFourierAlias(Ftmp);
	RCTOCREC(ReconTimer,ReconS_10);
	RCTICREC(ReconTimer,ReconS_11);
	FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Ftmp)
	{
		if (kp * kp + ip * ip + jp * jp < r_max * r_max)
		{
			DIRECT_A3D_ELEM(Ftmp, k, i, j) = FFTW_ELEM(Fconv, kp * padding_factor, ip * padding_factor, jp * padding_factor);
		}
		else
		{
			DIRECT_A3D_ELEM(Ftmp, k, i, j) = 0.;
		}
	}
	RCTOCREC(ReconTimer,ReconS_11);
	RCTICREC(ReconTimer,ReconS_12);
	// inverse FFT leaves result in vol_out
	transformer2.inverseFourierTransform();
	RCTOCREC(ReconTimer,ReconS_12);
	RCTICREC(ReconTimer,ReconS_13);
	// Shift the map back to its origin
	CenterFFT(vol_out, false);
	RCTOCREC(ReconTimer,ReconS_13);
	RCTICREC(ReconTimer,ReconS_14);
	// Un-normalize FFTW (because original FFTs were done with the size of 2D FFTs)
	if (ref_dim==3)
		vol_out /= ori_size;
	RCTOCREC(ReconTimer,ReconS_14);
	RCTICREC(ReconTimer,ReconS_15);
	// Mask out corners to prevent aliasing artefacts
	softMaskOutsideMap(vol_out);
	RCTOCREC(ReconTimer,ReconS_15);
	RCTICREC(ReconTimer,ReconS_16);
	// Gridding correction for the blob
	RFLOAT normftblob = tab_ftblob(0.);
	FOR_ALL_ELEMENTS_IN_ARRAY3D(vol_out)
	{

		RFLOAT r = sqrt((RFLOAT)(k*k+i*i+j*j));
		RFLOAT rval = r / (ori_size * padding_factor);
		A3D_ELEM(vol_out, k, i, j) /= tab_ftblob(rval) / normftblob;
		//if (k==0 && i==0)
		//	std::cerr << " j= " << j << " rval= " << rval << " tab_ftblob(rval) / normftblob= " << tab_ftblob(rval) / normftblob << std::endl;
	}
	RCTOCREC(ReconTimer,ReconS_16);

#else

	// rather than doing the blob-convolution to downsample the data array, do a windowing operation:
	// This is the same as convolution with a SINC. It seems to give better maps.
	// Then just make the blob look as much as a SINC as possible....
	// The "standard" r1.9, m2 and a15 blob looks quite like a sinc until the first zero (perhaps that's why it is standard?)
	//for (RFLOAT r = 0.1; r < 10.; r+=0.01)
	//{
	//	RFLOAT sinc = sin(PI * r / padding_factor ) / ( PI * r / padding_factor);
	//	std::cout << " r= " << r << " sinc= " << sinc << " blob= " << blob_val(r, blob) << std::endl;
	//}

	// Now do inverse FFT and window to original size in real-space
	// Pass the transformer to prevent making and clearing a new one before clearing the one declared above....
	// The latter may give memory problems as detected by electric fence....
	RCTICREC(ReconTimer,ReconS_17);

	// Size of padded real-space volume
	int padoridim = ROUND(padding_factor * ori_size);
	// make sure padoridim is even
	padoridim += padoridim%2;
    vol_out.reshape(padoridim, padoridim, padoridim);
    vol_out.setXmippOrigin();
	fullsize = padoridim *padoridim*padoridim;

	dividetask(numberZ,offsetZ,padoridim,GPU_N);

	multi_plan_init_transpose(plan,GPU_N,numberZ,offsetZ,padoridim);


	if(padoridim > pad_size)
	{
		for (int i = 0; i < GPU_N; ++i) {
			cudaSetDevice(plan[i].devicenum);
			cudaFree((plan[i].d_Data));
			cudaFree((plan[i].temp_Data));
			cudaMalloc((void**) &(plan[i].d_Data),sizeof(cufftComplex) * plan[i].datasize);
			cudaMalloc((void**) &(plan[i].temp_Data),sizeof(cufftComplex) * plan[i].tempsize);
		}

	}
	cufftComplex *c_Fconv;
	c_Fconv= (cufftComplex *)malloc(fullsize * sizeof(cufftComplex));
	windowFourierTransform(Fconv, padoridim);

	layoutchangecomp(Fconv.data,Fconv.xdim,Fconv.ydim,Fconv.zdim,padoridim,c_Fconv);

	xyN[0] = padoridim;
	xyN[1] = padoridim;
	Ndim[0]=padoridim;Ndim[1]=padoridim;Ndim[2]=padoridim;
    //==================2d fft plan

	for (int i = 0; i < GPU_N; i++) {
	    cudaSetDevice(plan[i].devicenum);
		cufftPlanMany(&xyplan[i], 2, xyN, NULL, 0, 0, NULL, 0, 0, CUFFT_C2C, numberZ[i]);
	}
   //==================1d fft plan

	for (int i = 0; i < GPU_N; i++) {
	    cudaSetDevice(plan[i].devicenum);
		int rank=1;
		int nrank[1];
		nrank[0]=Ndim[1];
		int inembed[2];
		inembed[0]=Ndim[0];
		inembed[1]=Ndim[1];
		cufftPlanMany(&zplan[i], rank, nrank, inembed, Ndim[0] , 1, inembed, Ndim[0], 1, CUFFT_C2C, Ndim[0]);
	}

	for(int i=0;i < GPU_N; i++)
	{
		cudaSetDevice(plan[i].devicenum);
		cudaMemcpy(plan[i].d_Data , c_Fconv + plan[i].selfoffset,(plan[i].datasize) * sizeof(cufftComplex),cudaMemcpyHostToDevice);
	}

	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(plan[i].devicenum);
		cufftExecC2C(xyplan[i], plan[i].d_Data ,plan[i].d_Data , CUFFT_INVERSE);

	}
	multi_sync(plan,GPU_N);

	yzlocal_transpose(plan,GPU_N,padoridim,offsetZ);
	transpose_exchange(plan,GPU_N,padoridim,offsetZ);
	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(plan[i].devicenum);
		int offset = 0;
		for(int zslice=0;zslice<numberZ[i];zslice++)
		{
			cufftExecC2C(zplan[i], plan[i].d_Data+offset, plan[i].d_Data+offset, CUFFT_INVERSE);
			offset+= padoridim*padoridim;
		}
	}
	yzlocal_transpose(plan,GPU_N,padoridim,offsetZ);
	transpose_exchange(plan,GPU_N,padoridim,offsetZ);
	for(int i=0;i < GPU_N; i++)
	{
		cudaSetDevice(plan[i].devicenum);
		cudaMemcpy( c_Fconv + plan[i].selfoffset,plan[i].d_Data ,(plan[i].datasize) * sizeof(cufftComplex),cudaMemcpyDeviceToHost);
	}

	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(plan[i].devicenum);
		cufftDestroy(zplan[i]);
		cufftDestroy(xyplan[i]);

		cudaFree(plan[i].d_Data);
		cudaFree(plan[i].temp_Data);
		cudaDeviceSynchronize();
	}
	free(xyplan);
	free(zplan);

	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(plan[i].devicenum);
		size_t freedata1,total1;
		cudaMemGetInfo( &freedata1, &total1 );
		printf("After alloccation  : %ld   %ld and gpu num %d \n",freedata1,total1,i);
	}


//	cudaMemcpy(plan[0].d_Data,c_Fconv,padoridim *padoridim*padoridim *sizeof(cufftComplex),cudaMemcpyHostToDevice);
//	cufftPlan3d( &cuffthandle2, padoridim, padoridim, padoridim , CUFFT_C2C);
//	cufftExecC2C(cuffthandle2, plan[0].d_Data, plan[0].d_Data, CUFFT_INVERSE);
	//cudaMemcpy(c_Fconv,plan[0].d_Data,padoridim *padoridim*padoridim *sizeof(cufftComplex),cudaMemcpyDeviceToHost);
    for(int i=0;i<padoridim *padoridim*padoridim;i++)
    	vol_out.data[i]=c_Fconv[i].x;

    printf("FINAL:");
    for(int i=0;i<10;i++)
    	printf("%f ",vol_out.data[i]);


    free(c_Fconv);

    CenterFFT(vol_out,true);

	// Window in real-space
	if (ref_dim==2)
	{
		vol_out.window(FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size),
				       LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size));
	}
	else
	{
		vol_out.window(FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size),
				       LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size));
	}
	vol_out.setXmippOrigin();

	// Normalisation factor of FFTW
	// The Fourier Transforms are all "normalised" for 2D transforms of size = ori_size x ori_size
	float normfft = (RFLOAT)(padding_factor * padding_factor * padding_factor * ori_size);
	vol_out /= normfft;
	// Mask out corners to prevent aliasing artefacts
	softMaskOutsideMap(vol_out);
	//windowToOridimRealSpace(transformer, vol_out, nr_threads, printTimes);
	RCTOCREC(ReconTimer,ReconS_17);

#endif

#ifdef DEBUG_RECONSTRUCT
	ttt()=vol_out;
	ttt.write("reconstruct_before_gridding_correction.spi");
#endif

	// Correct for the linear/nearest-neighbour interpolation that led to the data array
	RCTICREC(ReconTimer,ReconS_18);
	griddingCorrect(vol_out);
	RCTOCREC(ReconTimer,ReconS_18);
	// If the tau-values were calculated based on the FSC, then now re-calculate the power spectrum of the actual reconstruction
	if (update_tau2_with_fsc)
	{

		// New tau2 will be the power spectrum of the new map
		MultidimArray<RFLOAT> spectrum, count;

		// Calculate this map's power spectrum
		// Don't call getSpectrum() because we want to use the same transformer object to prevent memory trouble....
		RCTICREC(ReconTimer,ReconS_19);
		spectrum.initZeros(XSIZE(vol_out));
	    count.initZeros(XSIZE(vol_out));
		RCTOCREC(ReconTimer,ReconS_19);
		RCTICREC(ReconTimer,ReconS_20);
	    // recycle the same transformer for all images
        transformer.setReal(vol_out);
		RCTOCREC(ReconTimer,ReconS_20);
		RCTICREC(ReconTimer,ReconS_21);
        transformer.FourierTransform();
		RCTOCREC(ReconTimer,ReconS_21);
		RCTICREC(ReconTimer,ReconS_22);
	    FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
	    {
	    	long int idx = ROUND(sqrt(kp*kp + ip*ip + jp*jp));
	    	spectrum(idx) += norm(dAkij(Fconv, k, i, j));
	        count(idx) += 1.;
	    }
	    spectrum /= count;

		// Factor two because of two-dimensionality of the complex plane
		// (just like sigma2_noise estimates, the power spectra should be divided by 2)
		RFLOAT normfft = (ref_dim == 3 && data_dim == 2) ? (RFLOAT)(ori_size * ori_size) : 1.;
		spectrum *= normfft / 2.;

		// New SNR^MAP will be power spectrum divided by the noise in the reconstruction (i.e. sigma2)
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(data_vs_prior)
		{
			DIRECT_MULTIDIM_ELEM(tau2, n) =  tau2_fudge * DIRECT_MULTIDIM_ELEM(spectrum, n);
		}
		RCTOCREC(ReconTimer,ReconS_22);
	}
	RCTICREC(ReconTimer,ReconS_23);
	// Completely empty the transformer object
	transformer.cleanup();
    // Now can use extra mem to move data into smaller array space
    vol_out.shrinkToFit();

	RCTOCREC(ReconTimer,ReconS_23);
#ifdef TIMINGREC
    if(printTimes)
    	ReconTimer.printTimes(true);
#endif


#ifdef DEBUG_RECONSTRUCT
    std::cerr<<"done with reconstruct"<<std::endl;
#endif

	tau2_io = tau2;
    sigma2_out = sigma2;
    data_vs_prior_out = data_vs_prior;
    fourier_coverage_out = fourier_coverage;
}*/

void BackProjector::symmetrise(int nr_helical_asu, RFLOAT helical_twist, RFLOAT helical_rise)
{
	// First make sure the input arrays are obeying Hermitian symmetry,
	// which is assumed in the rotation operators of both helical and point group symmetry
	enforceHermitianSymmetry();

	// Then apply helical and point group symmetry (order irrelevant?)
	applyHelicalSymmetry(nr_helical_asu, helical_twist, helical_rise);

	applyPointGroupSymmetry();
}

void BackProjector::enforceHermitianSymmetry()
{
	for (int iz = STARTINGZ(data); iz <=FINISHINGZ(data); iz++)
	{
		// Make sure all points are only included once.
		int starty = (iz < 0) ? 0 : 1;
		for (int iy = starty; iy <= FINISHINGY(data); iy++)
		{
			// I just need to sum the two points, not divide by 2!
			Complex fsum = (A3D_ELEM(data, iz, iy, 0) + conj(A3D_ELEM(data, -iz, -iy, 0)));
			A3D_ELEM(data, iz, iy, 0) = fsum;
			A3D_ELEM(data, -iz, -iy, 0) = conj(fsum);
			RFLOAT sum = (A3D_ELEM(weight, iz, iy, 0) + A3D_ELEM(weight, -iz, -iy, 0));
			A3D_ELEM(weight, iz, iy, 0) = sum;
			A3D_ELEM(weight, -iz, -iy, 0) = sum;
		}
	}
}

void BackProjector::applyHelicalSymmetry(int nr_helical_asu, RFLOAT helical_twist, RFLOAT helical_rise)
{
	if ( (nr_helical_asu < 2) || (ref_dim != 3) )
		return;

	int rmax2 = ROUND(r_max * padding_factor) * ROUND(r_max * padding_factor);

	Matrix2D<RFLOAT> R(4, 4); // A matrix from the list
	MultidimArray<RFLOAT> sum_weight;
	MultidimArray<Complex > sum_data;
    RFLOAT x, y, z, fx, fy, fz, xp, yp, zp, r2;
    bool is_neg_x;
    int x0, x1, y0, y1, z0, z1;
	Complex d000, d001, d010, d011, d100, d101, d110, d111;
	Complex dx00, dx01, dx10, dx11, dxy0, dxy1, ddd;
	RFLOAT dd000, dd001, dd010, dd011, dd100, dd101, dd110, dd111;
	RFLOAT ddx00, ddx01, ddx10, ddx11, ddxy0, ddxy1;

    // First symmetry operator (not stored in SL) is the identity matrix
	sum_weight = weight;
	sum_data = data;
	int h_min = -nr_helical_asu/2;
	int h_max = -h_min + nr_helical_asu%2;
	for (int hh = h_min; hh < h_max; hh++)
	{
		if (hh != 0) // h==0 is done before the for loop (where sum_data = data)
		{
			RFLOAT rot_ang = hh * (-helical_twist);
			rotation3DMatrix(rot_ang, 'Z', R);
			R.setSmallValuesToZero(); // TODO: invert rotation matrix?

			// Loop over all points in the output (i.e. rotated, or summed) array
	        FOR_ALL_ELEMENTS_IN_ARRAY3D(sum_weight)
	        {

	        	x = (RFLOAT)j; // STARTINGX(sum_weight) is zero!
	        	y = (RFLOAT)i;
	        	z = (RFLOAT)k;
	        	r2 = x*x + y*y + z*z;
	        	if (r2 <= rmax2)
	        	{
	        		// coords_output(x,y) = A * coords_input (xp,yp)
					xp = x * R(0, 0) + y * R(0, 1) + z * R(0, 2);
					yp = x * R(1, 0) + y * R(1, 1) + z * R(1, 2);
					zp = x * R(2, 0) + y * R(2, 1) + z * R(2, 2);

					// Only asymmetric half is stored
					if (xp < 0)
					{
						// Get complex conjugated hermitian symmetry pair
						xp = -xp;
						yp = -yp;
						zp = -zp;
						is_neg_x = true;
					}
					else
					{
						is_neg_x = false;
					}

					// Trilinear interpolation (with physical coords)
					// Subtract STARTINGY and STARTINGZ to accelerate access to data (STARTINGX=0)
					// In that way use DIRECT_A3D_ELEM, rather than A3D_ELEM
	    			x0 = FLOOR(xp);
					fx = xp - x0;
					x1 = x0 + 1;

					y0 = FLOOR(yp);
					fy = yp - y0;
					y0 -=  STARTINGY(data);
					y1 = y0 + 1;

					z0 = FLOOR(zp);
					fz = zp - z0;
					z0 -= STARTINGZ(data);
					z1 = z0 + 1;

#ifdef CHECK_SIZE
					if (x0 < 0 || y0 < 0 || z0 < 0 ||
						x1 < 0 || y1 < 0 || z1 < 0 ||
						x0 >= XSIZE(data) || y0  >= YSIZE(data) || z0 >= ZSIZE(data) ||
						x1 >= XSIZE(data) || y1  >= YSIZE(data)  || z1 >= ZSIZE(data) 	)
					{
						std::cerr << " x0= " << x0 << " y0= " << y0 << " z0= " << z0 << std::endl;
						std::cerr << " x1= " << x1 << " y1= " << y1 << " z1= " << z1 << std::endl;
						data.printShape();
						REPORT_ERROR("BackProjector::applyPointGroupSymmetry: checksize!!!");
					}
#endif
					// First interpolate (complex) data
					d000 = DIRECT_A3D_ELEM(data, z0, y0, x0);
					d001 = DIRECT_A3D_ELEM(data, z0, y0, x1);
					d010 = DIRECT_A3D_ELEM(data, z0, y1, x0);
					d011 = DIRECT_A3D_ELEM(data, z0, y1, x1);
					d100 = DIRECT_A3D_ELEM(data, z1, y0, x0);
					d101 = DIRECT_A3D_ELEM(data, z1, y0, x1);
					d110 = DIRECT_A3D_ELEM(data, z1, y1, x0);
					d111 = DIRECT_A3D_ELEM(data, z1, y1, x1);

					dx00 = LIN_INTERP(fx, d000, d001);
					dx01 = LIN_INTERP(fx, d100, d101);
					dx10 = LIN_INTERP(fx, d010, d011);
					dx11 = LIN_INTERP(fx, d110, d111);
					dxy0 = LIN_INTERP(fy, dx00, dx10);
					dxy1 = LIN_INTERP(fy, dx01, dx11);

					// Take complex conjugated for half with negative x
					ddd = LIN_INTERP(fz, dxy0, dxy1);

					if (is_neg_x)
						ddd = conj(ddd);

					// Also apply a phase shift for helical translation along Z
					if (ABS(helical_rise) > 0.)
					{
						RFLOAT zshift = hh * helical_rise;
						zshift /= - ori_size * (RFLOAT)padding_factor;
						RFLOAT dotp = 2 * PI * (z * zshift);
						RFLOAT a = cos(dotp);
						RFLOAT b = sin(dotp);
						RFLOAT c = ddd.real;
						RFLOAT d = ddd.imag;
						RFLOAT ac = a * c;
						RFLOAT bd = b * d;
						RFLOAT ab_cd = (a + b) * (c + d);
						ddd = Complex(ac - bd, ab_cd - ac - bd);
					}
					// Accumulated sum of the data term
					A3D_ELEM(sum_data, k, i, j) += ddd;

					// Then interpolate (real) weight
					dd000 = DIRECT_A3D_ELEM(weight, z0, y0, x0);
					dd001 = DIRECT_A3D_ELEM(weight, z0, y0, x1);
					dd010 = DIRECT_A3D_ELEM(weight, z0, y1, x0);
					dd011 = DIRECT_A3D_ELEM(weight, z0, y1, x1);
					dd100 = DIRECT_A3D_ELEM(weight, z1, y0, x0);
					dd101 = DIRECT_A3D_ELEM(weight, z1, y0, x1);
					dd110 = DIRECT_A3D_ELEM(weight, z1, y1, x0);
					dd111 = DIRECT_A3D_ELEM(weight, z1, y1, x1);

					ddx00 = LIN_INTERP(fx, dd000, dd001);
					ddx01 = LIN_INTERP(fx, dd100, dd101);
					ddx10 = LIN_INTERP(fx, dd010, dd011);
					ddx11 = LIN_INTERP(fx, dd110, dd111);
					ddxy0 = LIN_INTERP(fy, ddx00, ddx10);
					ddxy1 = LIN_INTERP(fy, ddx01, ddx11);

					A3D_ELEM(sum_weight, k, i, j) +=  LIN_INTERP(fz, ddxy0, ddxy1);

	        	} // end if r2 <= rmax2

	        } // end loop over all elements of sum_weight

		} // end if hh!=0

	} // end loop over hh

	data = sum_data;
    weight = sum_weight;

}

void BackProjector::applyPointGroupSymmetry()
{

//#define DEBUG_SYMM
#ifdef DEBUG_SYMM
	std::cerr << " SL.SymsNo()= " << SL.SymsNo() << std::endl;
	std::cerr << " SL.true_symNo= " << SL.true_symNo << std::endl;
#endif

	int rmax2 = ROUND(r_max * padding_factor) * ROUND(r_max * padding_factor);
	if (SL.SymsNo() > 0 && ref_dim == 3)
	{
		Matrix2D<RFLOAT> L(4, 4), R(4, 4); // A matrix from the list
		MultidimArray<RFLOAT> sum_weight;
		MultidimArray<Complex > sum_data;
        RFLOAT x, y, z, fx, fy, fz, xp, yp, zp, r2;
        bool is_neg_x;
        int x0, x1, y0, y1, z0, z1;
    	Complex d000, d001, d010, d011, d100, d101, d110, d111;
    	Complex dx00, dx01, dx10, dx11, dxy0, dxy1;
    	RFLOAT dd000, dd001, dd010, dd011, dd100, dd101, dd110, dd111;
    	RFLOAT ddx00, ddx01, ddx10, ddx11, ddxy0, ddxy1;

        // First symmetry operator (not stored in SL) is the identity matrix
		sum_weight = weight;
		sum_data = data;
		// Loop over all other symmetry operators
	    for (int isym = 0; isym < SL.SymsNo(); isym++)
	    {
	        SL.get_matrices(isym, L, R);
#ifdef DEBUG_SYMM
	        std::cerr << " isym= " << isym << " R= " << R << std::endl;
#endif

	        // Loop over all points in the output (i.e. rotated, or summed) array
	        FOR_ALL_ELEMENTS_IN_ARRAY3D(sum_weight)
	        {

	        	x = (RFLOAT)j; // STARTINGX(sum_weight) is zero!
	        	y = (RFLOAT)i;
	        	z = (RFLOAT)k;
	        	r2 = x*x + y*y + z*z;
	        	if (r2 <= rmax2)
	        	{
	        		// coords_output(x,y) = A * coords_input (xp,yp)
					xp = x * R(0, 0) + y * R(0, 1) + z * R(0, 2);
					yp = x * R(1, 0) + y * R(1, 1) + z * R(1, 2);
					zp = x * R(2, 0) + y * R(2, 1) + z * R(2, 2);

					// Only asymmetric half is stored
					if (xp < 0)
					{
						// Get complex conjugated hermitian symmetry pair
						xp = -xp;
						yp = -yp;
						zp = -zp;
						is_neg_x = true;
					}
					else
					{
						is_neg_x = false;
					}

					// Trilinear interpolation (with physical coords)
					// Subtract STARTINGY and STARTINGZ to accelerate access to data (STARTINGX=0)
					// In that way use DIRECT_A3D_ELEM, rather than A3D_ELEM
	    			x0 = FLOOR(xp);
					fx = xp - x0;
					x1 = x0 + 1;

					y0 = FLOOR(yp);
					fy = yp - y0;
					y0 -=  STARTINGY(data);
					y1 = y0 + 1;

					z0 = FLOOR(zp);
					fz = zp - z0;
					z0 -= STARTINGZ(data);
					z1 = z0 + 1;

#ifdef CHECK_SIZE
					if (x0 < 0 || y0 < 0 || z0 < 0 ||
						x1 < 0 || y1 < 0 || z1 < 0 ||
						x0 >= XSIZE(data) || y0  >= YSIZE(data) || z0 >= ZSIZE(data) ||
						x1 >= XSIZE(data) || y1  >= YSIZE(data)  || z1 >= ZSIZE(data) 	)
					{
						std::cerr << " x0= " << x0 << " y0= " << y0 << " z0= " << z0 << std::endl;
						std::cerr << " x1= " << x1 << " y1= " << y1 << " z1= " << z1 << std::endl;
						data.printShape();
						REPORT_ERROR("BackProjector::applyPointGroupSymmetry: checksize!!!");
					}
#endif
					// First interpolate (complex) data
					d000 = DIRECT_A3D_ELEM(data, z0, y0, x0);
					d001 = DIRECT_A3D_ELEM(data, z0, y0, x1);
					d010 = DIRECT_A3D_ELEM(data, z0, y1, x0);
					d011 = DIRECT_A3D_ELEM(data, z0, y1, x1);
					d100 = DIRECT_A3D_ELEM(data, z1, y0, x0);
					d101 = DIRECT_A3D_ELEM(data, z1, y0, x1);
					d110 = DIRECT_A3D_ELEM(data, z1, y1, x0);
					d111 = DIRECT_A3D_ELEM(data, z1, y1, x1);

					dx00 = LIN_INTERP(fx, d000, d001);
					dx01 = LIN_INTERP(fx, d100, d101);
					dx10 = LIN_INTERP(fx, d010, d011);
					dx11 = LIN_INTERP(fx, d110, d111);
					dxy0 = LIN_INTERP(fy, dx00, dx10);
					dxy1 = LIN_INTERP(fy, dx01, dx11);

					// Take complex conjugated for half with negative x
					if (is_neg_x)
						A3D_ELEM(sum_data, k, i, j) += conj(LIN_INTERP(fz, dxy0, dxy1));
					else
						A3D_ELEM(sum_data, k, i, j) += LIN_INTERP(fz, dxy0, dxy1);

					// Then interpolate (real) weight
					dd000 = DIRECT_A3D_ELEM(weight, z0, y0, x0);
					dd001 = DIRECT_A3D_ELEM(weight, z0, y0, x1);
					dd010 = DIRECT_A3D_ELEM(weight, z0, y1, x0);
					dd011 = DIRECT_A3D_ELEM(weight, z0, y1, x1);
					dd100 = DIRECT_A3D_ELEM(weight, z1, y0, x0);
					dd101 = DIRECT_A3D_ELEM(weight, z1, y0, x1);
					dd110 = DIRECT_A3D_ELEM(weight, z1, y1, x0);
					dd111 = DIRECT_A3D_ELEM(weight, z1, y1, x1);

					ddx00 = LIN_INTERP(fx, dd000, dd001);
					ddx01 = LIN_INTERP(fx, dd100, dd101);
					ddx10 = LIN_INTERP(fx, dd010, dd011);
					ddx11 = LIN_INTERP(fx, dd110, dd111);
					ddxy0 = LIN_INTERP(fy, ddx00, ddx10);
					ddxy1 = LIN_INTERP(fy, ddx01, ddx11);

					A3D_ELEM(sum_weight, k, i, j) +=  LIN_INTERP(fz, ddxy0, ddxy1);

	        	} // end if r2 <= rmax2

	        } // end loop over all elements of sum_weight

	    } // end loop over symmetry operators

	    data = sum_data;
	    weight = sum_weight;
	    // Average
	    // The division should only be done if we would search all (C1) directions, not if we restrict the angular search!
	    /*
	    FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(data)
	    {
	    	DIRECT_MULTIDIM_ELEM(data, n) = DIRECT_MULTIDIM_ELEM(sum_data, n) / (RFLOAT)(SL.SymsNo() + 1);
	    	DIRECT_MULTIDIM_ELEM(weight, n) = DIRECT_MULTIDIM_ELEM(sum_weight, n) / (RFLOAT)(SL.SymsNo() + 1);
	    }
	    */
	}

}

void BackProjector::convoluteBlobRealSpace(FourierTransformer &transformer, bool do_mask, int threads)
{


	MultidimArray<RFLOAT> Mconv;
	int padhdim = pad_size / 2;

	// Set up right dimension of real-space array
	// TODO: resize this according to r_max!!!
	if (ref_dim==2)
		Mconv.reshape(pad_size, pad_size);
	else
		Mconv.reshape(pad_size, pad_size, pad_size);


	//printdatatofile(transformer.fFourier.data,transformer.fFourier.nzyxdim,transformer.fFourier.xdim,0,threads);
/*
	float maxdatareal=1;float mindatareal=1000000;
	float maxdataimag=1;float mindataimag=1000000;
	for(int i=0;i<transformer.fFourier.nzyxdim;i++)
	{
		if(transformer.fFourier.data[i].real>maxdatareal)
			maxdatareal=transformer.fFourier.data[i].real;

		if(transformer.fFourier.data[i].real!=0 && transformer.fFourier.data[i].real<mindatareal)
			mindatareal=transformer.fFourier.data[i].real;

		if(transformer.fFourier.data[i].imag>maxdataimag)
			maxdataimag=transformer.fFourier.data[i].imag;
		if(transformer.fFourier.data[i].imag!=0 && transformer.fFourier.data[i].imag<mindataimag)
			mindataimag=transformer.fFourier.data[i].imag;
	}
	printf("%f %f %f %f \n",maxdatareal,mindatareal,maxdataimag,mindataimag);*/


	transformer.setReal(Mconv);

	transformer.inverseFourierTransform();


/*
	printf("step2 from cpu : \n");
	for(int i=0;i<0+10;i++)
		printf("%f  \n",Mconv.data[i]);
	printf("\n");*/

/*	maxdatareal=1; mindatareal=1000000;

	for(int i=0;i<Mconv.nzyxdim;i++)
	{
		if(Mconv.data[i]>maxdatareal)
			maxdatareal=Mconv.data[i];

		if(Mconv.data[i]!=0 && Mconv.data[i]<mindatareal)
			mindatareal=Mconv.data[i];

	}
	printf("%f %f \n",maxdatareal,mindatareal);*/


	//printdatatofile(Mconv.data,Mconv.nzyxdim,Mconv.xdim,0,threads);


	// Blob normalisation in Fourier space
	RFLOAT normftblob = tab_ftblob(0.);

	//printf("%f \n",normftblob);
	// TMP DEBUGGING
	//struct blobtype blob;
	//blob.order = 0;
	//blob.radius = 1.9 * padding_factor;
	//blob.alpha = 15;

    // Multiply with FT of the blob kernel
	FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY3D(Mconv)
    {
		int kp = (k < padhdim) ? k : k - pad_size;
		int ip = (i < padhdim) ? i : i - pad_size;
		int jp = (j < padhdim) ? j : j - pad_size;
    	RFLOAT rval = sqrt ( (RFLOAT)(kp * kp + ip * ip + jp * jp) ) / (ori_size * padding_factor);
    	//if (kp==0 && ip==0 && jp > 0)
		//	std::cerr << " jp= " << jp << " rval= " << rval << " tab_ftblob(rval) / normftblob= " << tab_ftblob(rval) / normftblob << " ori_size/2= " << ori_size/2 << std::endl;
    	// In the final reconstruction: mask the real-space map beyond its original size to prevent aliasing ghosts
    	// Note that rval goes until 1/2 in the oversampled map
    	if (do_mask && rval > 1./(2. * padding_factor))
    		DIRECT_A3D_ELEM(Mconv, k, i, j) = 0.;
    	else
    		DIRECT_A3D_ELEM(Mconv, k, i, j) *= (tab_ftblob(rval) / normftblob);
    }


    transformer.FourierTransform();


}

void BackProjector::windowToOridimRealSpace(FourierTransformer &transformer, MultidimArray<RFLOAT> &Mout, int nr_threads, bool printTimes)
{

#ifdef TIMING
	Timer OriDimTimer;
	int OriDim1  = OriDimTimer.setNew(" OrD1_getFourier ");
	int OriDim2  = OriDimTimer.setNew(" OrD2_windowFFT ");
	int OriDim3  = OriDimTimer.setNew(" OrD3_reshape ");
	int OriDim4  = OriDimTimer.setNew(" OrD4_setReal ");
	int OriDim5  = OriDimTimer.setNew(" OrD5_invFFT ");
	int OriDim6  = OriDimTimer.setNew(" OrD6_centerFFT ");
	int OriDim7  = OriDimTimer.setNew(" OrD7_window ");
	int OriDim8  = OriDimTimer.setNew(" OrD8_norm ");
	int OriDim9  = OriDimTimer.setNew(" OrD9_softMask ");
#endif


	RCTIC(OriDimTimer,OriDim1);
	MultidimArray<Complex>& Fin = transformer.getFourierReference();
	RCTOC(OriDimTimer,OriDim1);
	RCTIC(OriDimTimer,OriDim2);
	MultidimArray<Complex > Ftmp;
	// Size of padded real-space volume
	int padoridim = ROUND(padding_factor * ori_size);
	// make sure padoridim is even
	padoridim += padoridim%2;
	RFLOAT normfft;

//#define DEBUG_WINDOWORIDIMREALSPACE
#ifdef DEBUG_WINDOWORIDIMREALSPACE
	Image<RFLOAT> tt;
	tt().reshape(ZSIZE(Fin), YSIZE(Fin), XSIZE(Fin));
	FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fin)
	{
		DIRECT_MULTIDIM_ELEM(tt(), n) = abs(DIRECT_MULTIDIM_ELEM(Fin, n));
	}
	tt.write("windoworidim_Fin.spi");
#endif

    // Resize incoming complex array to the correct size
	windowFourierTransform(Fin, padoridim);
	RCTOC(OriDimTimer,OriDim2);
	RCTIC(OriDimTimer,OriDim3);
 	if (ref_dim == 2)
	{
		Mout.reshape(padoridim, padoridim);
		if (data_dim == 2)
			normfft = (RFLOAT)(padding_factor * padding_factor);
		else
			normfft = (RFLOAT)(padding_factor * padding_factor * ori_size);
	}
	else
	{
		Mout.reshape(padoridim, padoridim, padoridim);
		if (data_dim == 3)
			normfft = (RFLOAT)(padding_factor * padding_factor * padding_factor);
		else
			normfft = (RFLOAT)(padding_factor * padding_factor * padding_factor * ori_size);
	}
	Mout.setXmippOrigin();
	RCTOC(OriDimTimer,OriDim3);

#ifdef DEBUG_WINDOWORIDIMREALSPACE
	tt().reshape(ZSIZE(Fin), YSIZE(Fin), XSIZE(Fin));
	FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fin)
	{
		DIRECT_MULTIDIM_ELEM(tt(), n) = abs(DIRECT_MULTIDIM_ELEM(Fin, n));
	}
	tt.write("windoworidim_Fresized.spi");
#endif

	// Do the inverse FFT
	RCTIC(OriDimTimer,OriDim4);
    transformer.setReal(Mout);
	RCTOC(OriDimTimer,OriDim4);
	RCTIC(OriDimTimer,OriDim5);
#ifdef TIMING
	if(printTimes)
		std::cout << std::endl << "FFTrealDims = (" << transformer.fReal->xdim << " , " << transformer.fReal->ydim << " , " << transformer.fReal->zdim << " ) " << std::endl;
#endif
	transformer.inverseFourierTransform();



	RCTOC(OriDimTimer,OriDim5);
    //transformer.inverseFourierTransform(Fin, Mout);
    Fin.clear();
    transformer.fReal = NULL; // Make sure to re-calculate fftw plan
	Mout.setXmippOrigin();

	printf("\nFInal \n");
	for(int i=0;i<10;i++)
		printf("%f ",Mout.data[i]);
	printf("\n");

	// Shift the map back to its origin

	RCTIC(OriDimTimer,OriDim6);
	CenterFFT(Mout,true);
	RCTOC(OriDimTimer,OriDim6);
#ifdef DEBUG_WINDOWORIDIMREALSPACE
	tt()=Mout;
	tt.write("windoworidim_Munwindowed.spi");
#endif

	// Window in real-space
	RCTIC(OriDimTimer,OriDim7);
	if (ref_dim==2)
	{
		Mout.window(FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size),
				       LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size));
	}
	else
	{
		Mout.window(FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size),
				       LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size));
	}
	Mout.setXmippOrigin();
	RCTOC(OriDimTimer,OriDim7);
	// Normalisation factor of FFTW
	// The Fourier Transforms are all "normalised" for 2D transforms of size = ori_size x ori_size
	RCTIC(OriDimTimer,OriDim8);

	Mout /= normfft;
	RCTOC(OriDimTimer,OriDim8);
#ifdef DEBUG_WINDOWORIDIMREALSPACE
	tt()=Mout;
	tt.write("windoworidim_Mwindowed.spi");
#endif

	// Mask out corners to prevent aliasing artefacts
	RCTIC(OriDimTimer,OriDim9);
	softMaskOutsideMap(Mout);
	RCTOC(OriDimTimer,OriDim9);

#ifdef DEBUG_WINDOWORIDIMREALSPACE
	tt()=Mout;
	tt.write("windoworidim_Mwindowed_masked.spi");
	FourierTransformer ttf;
	ttf.FourierTransform(Mout, Fin);
	tt().resize(ZSIZE(Fin), YSIZE(Fin), XSIZE(Fin));
	FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fin)
	{
		DIRECT_MULTIDIM_ELEM(tt(), n) = abs(DIRECT_MULTIDIM_ELEM(Fin, n));
	}
	tt.write("windoworidim_Fnew.spi");
#endif

#ifdef TIMING
    if(printTimes)
    	OriDimTimer.printTimes(true);
#endif

}
void BackProjector::reconstruct_gpu_transpose(MultidimArray<RFLOAT> &vol_out,
                                int max_iter_preweight,
                                bool do_map,
                                RFLOAT tau2_fudge,
                                MultidimArray<RFLOAT> &tau2_io, // can be input/output
                                MultidimArray<RFLOAT> &sigma2_out,
                                MultidimArray<RFLOAT> &data_vs_prior_out,
                                MultidimArray<RFLOAT> &fourier_coverage_out,
                                const MultidimArray<RFLOAT> &fsc, // only input
                                RFLOAT normalise,
                                bool update_tau2_with_fsc,
                                bool is_whole_instead_of_half,
                                int nr_threads,
                                int minres_map,
                                bool printTimes,
								bool do_fsc0999)
{

#ifdef TIMINGREC
	Timer ReconTimer;
	printTimes=1;
	int ReconS_1 = ReconTimer.setNew(" RcS1_Init ");
	int ReconS_2 = ReconTimer.setNew(" RcS2_Shape&Noise ");
	int ReconS_2_5 = ReconTimer.setNew(" RcS2.5_Regularize ");
	int ReconS_3 = ReconTimer.setNew(" RcS3_skipGridding ");
	int ReconS_4 = ReconTimer.setNew(" RcS4_doGridding_norm ");
	int ReconS_5 = ReconTimer.setNew(" RcS5_doGridding_init ");
	int ReconS_6 = ReconTimer.setNew(" RcS6_doGridding_iter ");
	int ReconS_7 = ReconTimer.setNew(" RcS7_doGridding_apply ");
	int ReconS_8 = ReconTimer.setNew(" RcS8_blobConvolute ");
	int ReconS_9 = ReconTimer.setNew(" RcS9_blobResize ");
	int ReconS_10 = ReconTimer.setNew(" RcS10_blobSetReal ");
	int ReconS_11 = ReconTimer.setNew(" RcS11_blobSetTemp ");
	int ReconS_12 = ReconTimer.setNew(" RcS12_blobTransform ");
	int ReconS_13 = ReconTimer.setNew(" RcS13_blobCenterFFT ");
	int ReconS_14 = ReconTimer.setNew(" RcS14_blobNorm1 ");
	int ReconS_15 = ReconTimer.setNew(" RcS15_blobSoftMask ");
	int ReconS_16 = ReconTimer.setNew(" RcS16_blobNorm2 ");
	int ReconS_17 = ReconTimer.setNew(" RcS17_WindowReal ");
	int ReconS_18 = ReconTimer.setNew(" RcS18_GriddingCorrect ");
	int ReconS_19 = ReconTimer.setNew(" RcS19_tauInit ");
	int ReconS_20 = ReconTimer.setNew(" RcS20_tausetReal ");
	int ReconS_21 = ReconTimer.setNew(" RcS21_tauTransform ");
	int ReconS_22 = ReconTimer.setNew(" RcS22_tautauRest ");
	int ReconS_23 = ReconTimer.setNew(" RcS23_tauShrinkToFit ");
	int ReconS_24 = ReconTimer.setNew(" RcS24_extra ");
#endif

    // never rely on references (handed to you from the outside) for computation:
    // they could be the same (i.e. reconstruct(..., dummy, dummy, dummy, dummy, ...); )

    MultidimArray<RFLOAT> sigma2, data_vs_prior, fourier_coverage;
	MultidimArray<RFLOAT> tau2 = tau2_io;


    RCTICREC(ReconTimer,ReconS_1);
    FourierTransformer transformer;
	MultidimArray<RFLOAT> Fweight;
	// Fnewweight can become too large for a float: always keep this one in double-precision
	MultidimArray<double> Fnewweight;
	MultidimArray<Complex>& Fconv = transformer.getFourierReference();
	int max_r2 = ROUND(r_max * padding_factor) * ROUND(r_max * padding_factor);

//#define DEBUG_RECONSTRUCT
#ifdef DEBUG_RECONSTRUCT
	Image<RFLOAT> ttt;
	FileName fnttt;
	ttt()=weight;
	ttt.write("reconstruct_initial_weight.spi");
	std::cerr << " pad_size= " << pad_size << " padding_factor= " << padding_factor << " max_r2= " << max_r2 << std::endl;
#endif

    // Set Fweight, Fnewweight and Fconv to the right size
    if (ref_dim == 2)
        vol_out.setDimensions(pad_size, pad_size, 1, 1);
    else
        // Too costly to actually allocate the space
        // Trick transformer with the right dimensions
        vol_out.setDimensions(pad_size, pad_size, pad_size, 1);

    transformer.setReal(vol_out); // Fake set real. 1. Allocate space for Fconv 2. calculate plans.
    vol_out.clear(); // Reset dimensions to 0

    RCTOCREC(ReconTimer,ReconS_1);
    RCTICREC(ReconTimer,ReconS_2);

    Fweight.reshape(Fconv);
    if (!skip_gridding)
    	Fnewweight.reshape(Fconv);

	// Go from projector-centered to FFTW-uncentered
	decenter(weight, Fweight, max_r2);

	// Take oversampling into account
	RFLOAT oversampling_correction = (ref_dim == 3) ? (padding_factor * padding_factor * padding_factor) : (padding_factor * padding_factor);
	MultidimArray<RFLOAT> counter;

	// First calculate the radial average of the (inverse of the) power of the noise in the reconstruction
	// This is the left-hand side term in the nominator of the Wiener-filter-like update formula
	// and it is stored inside the weight vector
	// Then, if (do_map) add the inverse of tau2-spectrum values to the weight
	sigma2.initZeros(ori_size/2 + 1);
	counter.initZeros(ori_size/2 + 1);
	FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
	{
		int r2 = kp * kp + ip * ip + jp * jp;
		if (r2 < max_r2)
		{
			int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
			RFLOAT invw = oversampling_correction * DIRECT_A3D_ELEM(Fweight, k, i, j);
			DIRECT_A1D_ELEM(sigma2, ires) += invw;
			DIRECT_A1D_ELEM(counter, ires) += 1.;
		}
    }

	// Average (inverse of) sigma2 in reconstruction
	FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(sigma2)
	{
        if (DIRECT_A1D_ELEM(sigma2, i) > 1e-10)
            DIRECT_A1D_ELEM(sigma2, i) = DIRECT_A1D_ELEM(counter, i) / DIRECT_A1D_ELEM(sigma2, i);
        else if (DIRECT_A1D_ELEM(sigma2, i) == 0)
            DIRECT_A1D_ELEM(sigma2, i) = 0.;
		else
		{
			std::cerr << " DIRECT_A1D_ELEM(sigma2, i)= " << DIRECT_A1D_ELEM(sigma2, i) << std::endl;
			REPORT_ERROR("BackProjector::reconstruct: ERROR: unexpectedly small, yet non-zero sigma2 value, this should not happen...a");
        }
    }

	if (update_tau2_with_fsc)
    {
        tau2.reshape(ori_size/2 + 1);
        data_vs_prior.initZeros(ori_size/2 + 1);
		// Then calculate new tau2 values, based on the FSC
		if (!fsc.sameShape(sigma2) || !fsc.sameShape(tau2))
		{
			fsc.printShape(std::cerr);
			tau2.printShape(std::cerr);
			sigma2.printShape(std::cerr);
			REPORT_ERROR("ERROR BackProjector::reconstruct: sigma2, tau2 and fsc have different sizes");
		}
		FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(sigma2)
        {
			// FSC cannot be negative or zero for conversion into tau2
			RFLOAT myfsc = XMIPP_MAX(0.001, DIRECT_A1D_ELEM(fsc, i));
			if (is_whole_instead_of_half)
			{
				// Factor two because of twice as many particles
				// Sqrt-term to get 60-degree phase errors....
				myfsc = sqrt(2. * myfsc / (myfsc + 1.));
			}
			myfsc = XMIPP_MIN(0.999, myfsc);
			RFLOAT myssnr = myfsc / (1. - myfsc);
			// Sjors 29nov2017 try tau2_fudge for pulling harder on Refine3D runs...
            myssnr *= tau2_fudge;
			RFLOAT fsc_based_tau = myssnr * DIRECT_A1D_ELEM(sigma2, i);
			DIRECT_A1D_ELEM(tau2, i) = fsc_based_tau;
			// data_vs_prior is merely for reporting: it is not used for anything in the reconstruction
			DIRECT_A1D_ELEM(data_vs_prior, i) = myssnr;
		}
	}
    RCTOCREC(ReconTimer,ReconS_2);
    RCTICREC(ReconTimer,ReconS_2_5);
	// Apply MAP-additional term to the Fnewweight array
	// This will regularise the actual reconstruction
    if (do_map)
	{

    	// Then, add the inverse of tau2-spectrum values to the weight
		// and also calculate spherical average of data_vs_prior ratios
		if (!update_tau2_with_fsc)
			data_vs_prior.initZeros(ori_size/2 + 1);
		fourier_coverage.initZeros(ori_size/2 + 1);
		counter.initZeros(ori_size/2 + 1);
		FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
 		{
			int r2 = kp * kp + ip * ip + jp * jp;
			if (r2 < max_r2)
			{
				int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
				RFLOAT invw = DIRECT_A3D_ELEM(Fweight, k, i, j);

				RFLOAT invtau2;
				if (DIRECT_A1D_ELEM(tau2, ires) > 0.)
				{
					// Calculate inverse of tau2
					invtau2 = 1. / (oversampling_correction * tau2_fudge * DIRECT_A1D_ELEM(tau2, ires));
				}
				else if (DIRECT_A1D_ELEM(tau2, ires) == 0.)
				{
					// If tau2 is zero, use small value instead
					invtau2 = 1./ ( 0.001 * invw);
				}
				else
				{
					std::cerr << " sigma2= " << sigma2 << std::endl;
					std::cerr << " fsc= " << fsc << std::endl;
					std::cerr << " tau2= " << tau2 << std::endl;
					REPORT_ERROR("ERROR BackProjector::reconstruct: Negative or zero values encountered for tau2 spectrum!");
				}

				// Keep track of spectral evidence-to-prior ratio and remaining noise in the reconstruction
				if (!update_tau2_with_fsc)
					DIRECT_A1D_ELEM(data_vs_prior, ires) += invw / invtau2;

				// Keep track of the coverage in Fourier space
				if (invw / invtau2 >= 1.)
					DIRECT_A1D_ELEM(fourier_coverage, ires) += 1.;

				DIRECT_A1D_ELEM(counter, ires) += 1.;

				// Only for (ires >= minres_map) add Wiener-filter like term
				if (ires >= minres_map)
				{
					// Now add the inverse-of-tau2_class term
					invw += invtau2;
					// Store the new weight again in Fweight
					DIRECT_A3D_ELEM(Fweight, k, i, j) = invw;
				}
			}
		}

		// Average data_vs_prior
		if (!update_tau2_with_fsc)
		{
			FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(data_vs_prior)
			{
				if (i > r_max)
					DIRECT_A1D_ELEM(data_vs_prior, i) = 0.;
				else if (DIRECT_A1D_ELEM(counter, i) < 0.001)
					DIRECT_A1D_ELEM(data_vs_prior, i) = 999.;
				else
					DIRECT_A1D_ELEM(data_vs_prior, i) /= DIRECT_A1D_ELEM(counter, i);
			}
		}

		// Calculate Fourier coverage in each shell
		FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(fourier_coverage)
		{
			if (DIRECT_A1D_ELEM(counter, i) > 0.)
				DIRECT_A1D_ELEM(fourier_coverage, i) /= DIRECT_A1D_ELEM(counter, i);
		}

	} //end if do_map
    else if (do_fsc0999)
    {

     	// Sjors 9may2018: avoid numerical instabilities with unregularised reconstructions....
        FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
        {
            int r2 = kp * kp + ip * ip + jp * jp;
            if (r2 < max_r2)
            {
                int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
                if (ires >= minres_map)
                {
                    // add 1/1000th of the radially averaged sigma2 to the Fweight, to avoid having zeros there...
                	DIRECT_A3D_ELEM(Fweight, k, i, j) += 1./(999. * DIRECT_A1D_ELEM(sigma2, ires));
                }
            }
        }

    }

//==============================================================================add  GPU version

	int Ndim[3];
	int devCount;
	cudaGetDeviceCount(&devCount);
	int GPU_N=devCount;

	Ndim[0]=pad_size;
	Ndim[1]=pad_size;
	Ndim[2]=pad_size;
	size_t fullsize= pad_size*pad_size*pad_size;
    initgpu(GPU_N);

	int offset;
	//divide
	int *numberZ = (int *)malloc(sizeof(int)*GPU_N);
	int *offsetZ = (int *)malloc(sizeof(int)*GPU_N);
	dividetask(numberZ,offsetZ,pad_size,GPU_N);

	size_t padtrans_size = numberZ[0]*GPU_N;
	int *numbertmpZ;
	int *offsettmpZ;
	numbertmpZ = (int *)malloc(sizeof(int)*GPU_N);
	offsettmpZ = (int *)malloc(sizeof(int)*GPU_N);
	dividetask(numbertmpZ,offsettmpZ,padtrans_size,GPU_N);


	MultiGPUplan *plan;
	plan = (MultiGPUplan *)malloc(sizeof(MultiGPUplan)*GPU_N);
	multi_plan_init_transpose(plan,GPU_N,numberZ,offsetZ,pad_size);



	for (int i = 0; i < GPU_N; ++i) {
		cudaSetDevice(plan[i].devicenum);
		cudaMalloc((void**) & (plan[i].d_Data),sizeof(cufftComplex) * plan[i].datasize);
		cudaMalloc((void**) & (plan[i].temp_Data), sizeof(cufftComplex) * plan[i].tempsize);
	}

	//=================================
	int xyN[2];
	xyN[0] = Ndim[0];
	xyN[1] = Ndim[1];
	cufftHandle *xyplan;
	xyplan = (cufftHandle*) malloc(sizeof(cufftHandle)*GPU_N);
	cufftHandle *zplan;
	zplan = (cufftHandle*) malloc(sizeof(cufftHandle)*GPU_N);


    RCTOCREC(ReconTimer,ReconS_2_5);
	if (skip_gridding)
	{
	    RCTICREC(ReconTimer,ReconS_3);
		std::cerr << "Skipping gridding!" << std::endl;
		Fconv.initZeros(); // to remove any stuff from the input volume
		decenter(data, Fconv, max_r2);

		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			if (DIRECT_MULTIDIM_ELEM(Fweight, n) > 0.)
				DIRECT_MULTIDIM_ELEM(Fconv, n) /= DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		RCTOCREC(ReconTimer,ReconS_3);
#ifdef DEBUG_RECONSTRUCT
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			DIRECT_MULTIDIM_ELEM(ttt(), n) = DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		ttt.write("reconstruct_skipgridding_correction_term.spi");
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			if (DIRECT_MULTIDIM_ELEM(Fweight, n) > 0.)
				DIRECT_MULTIDIM_ELEM(ttt(), n) = 1./DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		ttt.write("reconstruct_skipgridding_correction_term_inverse.spi");
#endif
	}
	else
	{
		RCTICREC(ReconTimer,ReconS_4);
		// Divide both data and Fweight by normalisation factor to prevent FFT's with very large values....
	#ifdef DEBUG_RECONSTRUCT
		std::cerr << " normalise= " << normalise << std::endl;
	#endif
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fweight)
		{
			DIRECT_MULTIDIM_ELEM(Fweight, n) /= normalise;
		}
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(data)
		{
			DIRECT_MULTIDIM_ELEM(data, n) /= normalise;
		}
		RCTOCREC(ReconTimer,ReconS_4);
		RCTICREC(ReconTimer,ReconS_5);
        // Initialise Fnewweight with 1's and 0's. (also see comments below)
		FOR_ALL_ELEMENTS_IN_ARRAY3D(weight)
		{
			if (k * k + i * i + j * j < max_r2)
				A3D_ELEM(weight, k, i, j) = 1.;
			else
				A3D_ELEM(weight, k, i, j) = 0.;
		}



		decenter(weight, Fnewweight, max_r2);
		RCTOCREC(ReconTimer,ReconS_5);
		// Iterative algorithm as in  Eq. [14] in Pipe & Menon (1999)
		// or Eq. (4) in Matej (2001)


		long int Fconvnum=Fconv.nzyxdim;
		long int normsize= Ndim[0]*Ndim[1]*Ndim[2];
		int halfxdim=Fconv.xdim;

		printf("%ld %ld %ld \n",Fconv.xdim,Fconv.ydim,Fconv.zdim);
		printf("Fnewweight: %ld %ld %ld \n",Fnewweight.xdim,Fnewweight.ydim,Fnewweight.zdim);
		multi_enable_access(plan,GPU_N);


         //==================2d fft plan

		for (int i = 0; i < GPU_N; i++) {
		    cudaSetDevice(plan[i].devicenum);
			cufftPlanMany(&xyplan[i], 2, xyN, NULL, 0, 0, NULL, 0, 0, CUFFT_C2C, numberZ[i]);
		}
	    //==================1d fft plan

		for (int i = 0; i < GPU_N; i++) {
		    cudaSetDevice(plan[i].devicenum);
			int rank=1;
			int nrank[1];
			nrank[0]=Ndim[1];
			int inembed[2];
			inembed[0]=Ndim[0];
			inembed[1]=Ndim[1];
			cufftPlanMany(&zplan[i], rank, nrank, inembed, Ndim[0] , 1, inembed, Ndim[0], 1, CUFFT_C2C, Ndim[0]);
		}


		float *fweightdata= (float *)malloc(sizeof(float)*pad_size*pad_size*pad_size);
		layoutchange(Fweight.data,Fweight.xdim,Fweight.ydim,Fweight.zdim,pad_size,fweightdata);
		float **d_blocktwo;
		d_blocktwo =(float **)malloc(GPU_N*sizeof(float*));
		for(int i=0;i< GPU_N;i++)
		{
			cudaSetDevice(plan[i].devicenum);
			cudaMalloc((void**) &(d_blocktwo[i]),sizeof(float) * plan[i].realsize);
			cudaMemcpy(d_blocktwo[i],fweightdata+plan[i].selfoffset,plan[i].realsize*sizeof(float),cudaMemcpyHostToDevice);
		}
		multi_sync(plan,GPU_N);
		free(fweightdata);


		double *tempdata= (double *)malloc(sizeof(double)*pad_size*pad_size*pad_size);
		// every thread map two block to used
		layoutchange(Fnewweight.data,Fnewweight.xdim,Fnewweight.ydim,Fnewweight.zdim,pad_size,tempdata);
		for(int i=0;i<pad_size;i++)
		{
			for(int j=0;j<pad_size;j++)

			{
				for(int k=0;k<pad_size;k++)
					printf("%.2f ",tempdata[i*pad_size*pad_size+j*pad_size+k]);
				printf("\n");
			}
			printf("\n");
		}


		double **d_blockone;
		d_blockone = (double **)malloc(GPU_N*sizeof(double *));

		for (int i = 0; i < GPU_N; i++) {
		    cudaSetDevice(plan[i].devicenum);
			cudaMalloc((void**) &(d_blockone[i]),sizeof(double) * plan[i].realsize);
			cudaMemcpy(d_blockone[i],tempdata+plan[i].selfoffset,plan[i].realsize*sizeof(double),cudaMemcpyHostToDevice);
		}
		multi_sync(plan,GPU_N);



		cufftComplex *cpu_data=(cufftComplex* )malloc(sizeof(cufftComplex)*pad_size*pad_size);

		for (int iter = 0; iter < max_iter_preweight; iter++)
		{

            //std::cout << "    iteration " << (iter+1) << "/" << max_iter_preweight << "\n";
			RCTICREC(ReconTimer,ReconS_6);

			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(plan[i].devicenum);
				vector_Multi_layout_mpi(d_blockone[i],d_blocktwo[i],plan[i].d_Data, plan[i].realsize);
				cufftExecC2C(xyplan[i], plan[i].d_Data,plan[i].d_Data , CUFFT_INVERSE);
				cudaDeviceSynchronize();
			}



				cudaMemcpy(cpu_data,plan[1].d_Data,pad_size*pad_size*sizeof(cufftComplex),cudaMemcpyDeviceToHost);
				printf("1\n");
				for(int i=0;i<pad_size;i++)
					//for(int j=0;j<pad_size;j++)
					printf("%f ",cpu_data[i*pad_size].x);
				printf("\n");




			yzlocal_transpose(plan,GPU_N,pad_size,numberZ,offsetZ,offsettmpZ);
			transpose_exchange(plan,GPU_N,pad_size,offsetZ,offsettmpZ);


			cudaMemcpy(cpu_data,plan[1].d_Data,pad_size*pad_size*sizeof(cufftComplex),cudaMemcpyDeviceToHost);
			printf("2\n");
			for(int i=0;i<pad_size;i++)
				//for(int j=0;j<pad_size;j++)
				printf("%f ",cpu_data[i*pad_size].x);
			printf("\n");



			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(plan[i].devicenum);
				int offset = 0;
				for(int zslice=0;zslice<numberZ[i];zslice++)
				{
					cufftExecC2C(zplan[i], plan[i].d_Data+offset, plan[i].d_Data+offset, CUFFT_INVERSE);
					offset+= pad_size*pad_size;
				}
			}


			cudaMemcpy(cpu_data,plan[1].d_Data,pad_size*pad_size*sizeof(cufftComplex),cudaMemcpyDeviceToHost);
			printf("3\n");
			for(int i=0;i<pad_size;i++)
				//for(int j=0;j<pad_size;j++)
				printf("%f ",cpu_data[i*pad_size].x);
			printf("\n");
			//transpose again
			yzlocal_transpose(plan,GPU_N,pad_size,numberZ,offsetZ,offsettmpZ);
			transpose_exchange(plan,GPU_N,pad_size,offsetZ,offsettmpZ);



			RFLOAT normftblob = tab_ftblob(0.);
			RFLOAT *d_tab_ftblob;
			int tabxdim=tab_ftblob.tabulatedValues.xdim;
#ifdef RELION_SINGLE_PRECISION
			d_tab_ftblob=gpusetdata_float(d_tab_ftblob,tab_ftblob.tabulatedValues.xdim,tab_ftblob.tabulatedValues.data);
#else
			d_tab_ftblob=gpusetdata_double(d_tab_ftblob,tab_ftblob.tabulatedValues.xdim,tab_ftblob.tabulatedValues.data);
#endif
			//gpu_kernel2
			for(int i = 0; i < GPU_N; i++)
			{
				cudaSetDevice(plan[i].devicenum);
				volume_Multi_float_transone(plan[i].d_Data,d_tab_ftblob, Ndim[0]*Ndim[1]*numberZ[i],
									tabxdim, tab_ftblob.sampling , pad_size/2, pad_size, ori_size, padding_factor, normftblob,Ndim[1],offsetZ[i]);
			}



			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(plan[i].devicenum);
				cufftExecC2C(xyplan[i], plan[i].d_Data,plan[i].d_Data , CUFFT_FORWARD);
				cudaDeviceSynchronize();
			}

			yzlocal_transpose(plan,GPU_N,pad_size,numberZ,offsetZ,offsettmpZ);
			transpose_exchange(plan,GPU_N,pad_size,offsetZ,offsettmpZ);
			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(plan[i].devicenum);
				int offset = 0;
				for(int zslice=0;zslice<numberZ[i];zslice++)
				{
					cufftExecC2C(zplan[i], plan[i].d_Data+offset, plan[i].d_Data+offset, CUFFT_FORWARD);
					offset+= pad_size*pad_size;
				}
				cudaDeviceSynchronize();
			}

			//transpose again

			yzlocal_transpose(plan,GPU_N,pad_size,numberZ,offsetZ,offsettmpZ);
			transpose_exchange(plan,GPU_N,pad_size,offsetZ,offsettmpZ);

			for (int i = 0; i < GPU_N; i++) {
				cudaSetDevice(plan[i].devicenum);
				vector_Normlize(plan[i].d_Data, normsize ,Ndim[0]*Ndim[1]*numberZ[i]);
				//gpu_kernel3
				fft_Divide_mpi(plan[i].d_Data,d_blockone[i],plan[i].realsize,pad_size*pad_size,pad_size,pad_size,pad_size,Fconv.xdim,max_r2,offsetZ[i]);
			}

			RCTOCREC(ReconTimer,ReconS_6);
			//cudaMemcpy(c_Fconv,plan[0].d_Data,100*sizeof(cufftComplex),cudaMemcpyDeviceToHost);
			//printf("step iter end  from gpu : \n");

	#ifdef DEBUG_RECONSTRUCT
			std::cerr << " PREWEIGHTING ITERATION: "<< iter + 1 << " OF " << max_iter_preweight << std::endl;
			// report of maximum and minimum values of current conv_weight
			std::cerr << " corr_avg= " << corr_avg / corr_nn << std::endl;
			std::cerr << " corr_min= " << corr_min << std::endl;
			std::cerr << " corr_max= " << corr_max << std::endl;
	#endif

		}

		for(int i=0;i<GPU_N; i++){
			cudaSetDevice(plan[i].devicenum);
			cudaMemcpy(tempdata+plan[i].selfoffset,d_blockone[i],plan[i].realsize*sizeof(double),cudaMemcpyDeviceToHost);
		}
		layoutchangeback(tempdata,Fnewweight.xdim,Fnewweight.ydim,Fnewweight.zdim,pad_size,Fnewweight.data);


		printf("TEMP res :\n");
		for(int i=0;i<10;i++)
			printf("%f ",Fnewweight.data[i]);


		for (int i = 0; i < GPU_N; i++) {
			cudaSetDevice(plan[i].devicenum);
			cufftDestroy(xyplan[i]);
			cufftDestroy(zplan[i]);
			cudaFree(d_blockone[i]);
			cudaFree(d_blocktwo[i]);
		}
		free(tempdata);
		free(d_blockone);
		free(d_blocktwo);
		RCTICREC(ReconTimer,ReconS_7);

		for (int i = 0; i < GPU_N; i++) {
			cudaSetDevice(plan[i].devicenum);
			size_t freedata1,total1;
			cudaMemGetInfo( &freedata1, &total1 );
			printf("After alloccation loop : %ld   %ld and gpu num %d \n",freedata1,total1,i);
		}

	#ifdef DEBUG_RECONSTRUCT
		Image<double> tttt;
		tttt()=Fnewweight;
		tttt.write("reconstruct_gridding_weight.spi");
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			DIRECT_MULTIDIM_ELEM(ttt(), n) = abs(DIRECT_MULTIDIM_ELEM(Fconv, n));
		}
		ttt.write("reconstruct_gridding_correction_term.spi");
	#endif


		// Clear memory
		Fweight.clear();

		// Note that Fnewweight now holds the approximation of the inverse of the weights on a regular grid

		// Now do the actual reconstruction with the data array
		// Apply the iteratively determined weight
		Fconv.initZeros(); // to remove any stuff from the input volume
		decenter(data, Fconv, max_r2);
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
#ifdef  RELION_SINGLE_PRECISION
			// Prevent numerical instabilities in single-precision reconstruction with very unevenly sampled orientations
			if (DIRECT_MULTIDIM_ELEM(Fnewweight, n) > 1e20)
				DIRECT_MULTIDIM_ELEM(Fnewweight, n) = 1e20;
#endif
			DIRECT_MULTIDIM_ELEM(Fconv, n) *= DIRECT_MULTIDIM_ELEM(Fnewweight, n);
		}

		// Clear memory
		Fnewweight.clear();
		RCTOCREC(ReconTimer,ReconS_7);
	} // end if skip_gridding

// Gridding theory says one now has to interpolate the fine grid onto the coarse one using a blob kernel
// and then do the inverse transform and divide by the FT of the blob (i.e. do the gridding correction)
// In practice, this gives all types of artefacts (perhaps I never found the right implementation?!)
// Therefore, window the Fourier transform and then do the inverse transform
//#define RECONSTRUCT_CONVOLUTE_BLOB
#ifdef RECONSTRUCT_CONVOLUTE_BLOB

	// Apply the same blob-convolution as above to the data array
	// Mask real-space map beyond its original size to prevent aliasing in the downsampling step below
	RCTICREC(ReconTimer,ReconS_8);
	convoluteBlobRealSpace(transformer, true);
	RCTOCREC(ReconTimer,ReconS_8);
	RCTICREC(ReconTimer,ReconS_9);
	// Now just pick every 3rd pixel in Fourier-space (i.e. down-sample)
	// and do a final inverse FT
	if (ref_dim == 2)
		vol_out.resize(ori_size, ori_size);
	else
		vol_out.resize(ori_size, ori_size, ori_size);
	RCTOCREC(ReconTimer,ReconS_9);
	RCTICREC(ReconTimer,ReconS_10);
	FourierTransformer transformer2;
	MultidimArray<Complex > Ftmp;
	transformer2.setReal(vol_out); // cannot use the first transformer because Fconv is inside there!!
	transformer2.getFourierAlias(Ftmp);
	RCTOCREC(ReconTimer,ReconS_10);
	RCTICREC(ReconTimer,ReconS_11);
	FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Ftmp)
	{
		if (kp * kp + ip * ip + jp * jp < r_max * r_max)
		{
			DIRECT_A3D_ELEM(Ftmp, k, i, j) = FFTW_ELEM(Fconv, kp * padding_factor, ip * padding_factor, jp * padding_factor);
		}
		else
		{
			DIRECT_A3D_ELEM(Ftmp, k, i, j) = 0.;
		}
	}
	RCTOCREC(ReconTimer,ReconS_11);
	RCTICREC(ReconTimer,ReconS_12);
	// inverse FFT leaves result in vol_out
	transformer2.inverseFourierTransform();
	RCTOCREC(ReconTimer,ReconS_12);
	RCTICREC(ReconTimer,ReconS_13);
	// Shift the map back to its origin
	CenterFFT(vol_out, false);
	RCTOCREC(ReconTimer,ReconS_13);
	RCTICREC(ReconTimer,ReconS_14);
	// Un-normalize FFTW (because original FFTs were done with the size of 2D FFTs)
	if (ref_dim==3)
		vol_out /= ori_size;
	RCTOCREC(ReconTimer,ReconS_14);
	RCTICREC(ReconTimer,ReconS_15);
	// Mask out corners to prevent aliasing artefacts
	softMaskOutsideMap(vol_out);
	RCTOCREC(ReconTimer,ReconS_15);
	RCTICREC(ReconTimer,ReconS_16);
	// Gridding correction for the blob
	RFLOAT normftblob = tab_ftblob(0.);
	FOR_ALL_ELEMENTS_IN_ARRAY3D(vol_out)
	{

		RFLOAT r = sqrt((RFLOAT)(k*k+i*i+j*j));
		RFLOAT rval = r / (ori_size * padding_factor);
		A3D_ELEM(vol_out, k, i, j) /= tab_ftblob(rval) / normftblob;
		//if (k==0 && i==0)
		//	std::cerr << " j= " << j << " rval= " << rval << " tab_ftblob(rval) / normftblob= " << tab_ftblob(rval) / normftblob << std::endl;
	}
	RCTOCREC(ReconTimer,ReconS_16);

#else

	// rather than doing the blob-convolution to downsample the data array, do a windowing operation:
	// This is the same as convolution with a SINC. It seems to give better maps.
	// Then just make the blob look as much as a SINC as possible....
	// The "standard" r1.9, m2 and a15 blob looks quite like a sinc until the first zero (perhaps that's why it is standard?)
	//for (RFLOAT r = 0.1; r < 10.; r+=0.01)
	//{
	//	RFLOAT sinc = sin(PI * r / padding_factor ) / ( PI * r / padding_factor);
	//	std::cout << " r= " << r << " sinc= " << sinc << " blob= " << blob_val(r, blob) << std::endl;
	//}

	// Now do inverse FFT and window to original size in real-space
	// Pass the transformer to prevent making and clearing a new one before clearing the one declared above....
	// The latter may give memory problems as detected by electric fence....
	RCTICREC(ReconTimer,ReconS_17);

	// Size of padded real-space volume
	int padoridim = ROUND(padding_factor * ori_size);
	// make sure padoridim is even
	padoridim += padoridim%2;
    vol_out.reshape(padoridim, padoridim, padoridim);
    vol_out.setXmippOrigin();
	fullsize = padoridim *padoridim*padoridim;

	dividetask(numberZ,offsetZ,padoridim,GPU_N);

	multi_plan_init_transpose(plan,GPU_N,numberZ,offsetZ,padoridim);


	if(padoridim > pad_size)
	{
		for (int i = 0; i < GPU_N; ++i) {
			cudaSetDevice(plan[i].devicenum);
			cudaFree((plan[i].d_Data));
			cudaFree((plan[i].temp_Data));
			cudaMalloc((void**) &(plan[i].d_Data),sizeof(cufftComplex) * plan[i].datasize);
			cudaMalloc((void**) &(plan[i].temp_Data),sizeof(cufftComplex) * plan[i].tempsize);
		}



	}
	cufftComplex *c_Fconv;
	c_Fconv= (cufftComplex *)malloc(fullsize * sizeof(cufftComplex));
	windowFourierTransform(Fconv, padoridim);

	layoutchangecomp(Fconv.data,Fconv.xdim,Fconv.ydim,Fconv.zdim,padoridim,c_Fconv);

	xyN[0] = padoridim;
	xyN[1] = padoridim;
	Ndim[0]=padoridim;Ndim[1]=padoridim;Ndim[2]=padoridim;
    //==================2d fft plan

	for (int i = 0; i < GPU_N; i++) {
	    cudaSetDevice(plan[i].devicenum);
		cufftPlanMany(&xyplan[i], 2, xyN, NULL, 0, 0, NULL, 0, 0, CUFFT_C2C, numberZ[i]);
	}
   //==================1d fft plan

	for (int i = 0; i < GPU_N; i++) {
	    cudaSetDevice(plan[i].devicenum);
		int rank=1;
		int nrank[1];
		nrank[0]=Ndim[1];
		int inembed[2];
		inembed[0]=Ndim[0];
		inembed[1]=Ndim[1];
		cufftPlanMany(&zplan[i], rank, nrank, inembed, Ndim[0] , 1, inembed, Ndim[0], 1, CUFFT_C2C, Ndim[0]);
	}

	for(int i=0;i < GPU_N; i++)
	{
		cudaSetDevice(plan[i].devicenum);
		cudaMemcpy(plan[i].d_Data , c_Fconv + plan[i].selfoffset,(plan[i].datasize) * sizeof(cufftComplex),cudaMemcpyHostToDevice);
	}

	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(plan[i].devicenum);
		cufftExecC2C(xyplan[i], plan[i].d_Data ,plan[i].d_Data , CUFFT_INVERSE);

	}
	multi_sync(plan,GPU_N);

	yzlocal_transpose(plan,GPU_N,padoridim,numberZ,offsetZ,offsettmpZ);
	transpose_exchange(plan,GPU_N,padoridim,offsetZ,offsettmpZ);
	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(plan[i].devicenum);
		int offset = 0;
		for(int zslice=0;zslice<numberZ[i];zslice++)
		{
			cufftExecC2C(zplan[i], plan[i].d_Data+offset, plan[i].d_Data+offset, CUFFT_INVERSE);
			offset+= padoridim*padoridim;
		}
	}
	yzlocal_transpose(plan,GPU_N,padoridim,numberZ,offsetZ,offsettmpZ);
	transpose_exchange(plan,GPU_N,padoridim,offsetZ,offsettmpZ);
	for(int i=0;i < GPU_N; i++)
	{
		cudaSetDevice(plan[i].devicenum);
		cudaMemcpy( c_Fconv + plan[i].selfoffset,plan[i].d_Data ,(plan[i].datasize) * sizeof(cufftComplex),cudaMemcpyDeviceToHost);
	}

	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(plan[i].devicenum);
		cufftDestroy(zplan[i]);
		cufftDestroy(xyplan[i]);

		cudaFree(plan[i].d_Data);
		cudaFree(plan[i].temp_Data);
		cudaDeviceSynchronize();
	}
	free(xyplan);
	free(zplan);

	for (int i = 0; i < GPU_N; i++) {
		cudaSetDevice(plan[i].devicenum);
		size_t freedata1,total1;
		cudaMemGetInfo( &freedata1, &total1 );
		printf("After alloccation  : %ld   %ld and gpu num %d \n",freedata1,total1,i);
	}


//	cudaMemcpy(plan[0].d_Data,c_Fconv,padoridim *padoridim*padoridim *sizeof(cufftComplex),cudaMemcpyHostToDevice);
//	cufftPlan3d( &cuffthandle2, padoridim, padoridim, padoridim , CUFFT_C2C);
//	cufftExecC2C(cuffthandle2, plan[0].d_Data, plan[0].d_Data, CUFFT_INVERSE);
	//cudaMemcpy(c_Fconv,plan[0].d_Data,padoridim *padoridim*padoridim *sizeof(cufftComplex),cudaMemcpyDeviceToHost);
    for(int i=0;i<padoridim *padoridim*padoridim;i++)
    	vol_out.data[i]=c_Fconv[i].x;

    printf("FINAL:");
    for(int i=0;i<10;i++)
    	printf("%f ",vol_out.data[i]);


    free(c_Fconv);

    CenterFFT(vol_out,true);

	// Window in real-space
	if (ref_dim==2)
	{
		vol_out.window(FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size),
				       LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size));
	}
	else
	{
		vol_out.window(FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size), FIRST_XMIPP_INDEX(ori_size),
				       LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size), LAST_XMIPP_INDEX(ori_size));
	}
	vol_out.setXmippOrigin();

	// Normalisation factor of FFTW
	// The Fourier Transforms are all "normalised" for 2D transforms of size = ori_size x ori_size
	float normfft = (RFLOAT)(padding_factor * padding_factor * padding_factor * ori_size);
	vol_out /= normfft;
	// Mask out corners to prevent aliasing artefacts
	softMaskOutsideMap(vol_out);
	//windowToOridimRealSpace(transformer, vol_out, nr_threads, printTimes);
	RCTOCREC(ReconTimer,ReconS_17);

#endif

#ifdef DEBUG_RECONSTRUCT
	ttt()=vol_out;
	ttt.write("reconstruct_before_gridding_correction.spi");
#endif

	// Correct for the linear/nearest-neighbour interpolation that led to the data array
	RCTICREC(ReconTimer,ReconS_18);
	griddingCorrect(vol_out);
	RCTOCREC(ReconTimer,ReconS_18);
	// If the tau-values were calculated based on the FSC, then now re-calculate the power spectrum of the actual reconstruction
	if (update_tau2_with_fsc)
	{

		// New tau2 will be the power spectrum of the new map
		MultidimArray<RFLOAT> spectrum, count;

		// Calculate this map's power spectrum
		// Don't call getSpectrum() because we want to use the same transformer object to prevent memory trouble....
		RCTICREC(ReconTimer,ReconS_19);
		spectrum.initZeros(XSIZE(vol_out));
	    count.initZeros(XSIZE(vol_out));
		RCTOCREC(ReconTimer,ReconS_19);
		RCTICREC(ReconTimer,ReconS_20);
	    // recycle the same transformer for all images
        transformer.setReal(vol_out);
		RCTOCREC(ReconTimer,ReconS_20);
		RCTICREC(ReconTimer,ReconS_21);
        transformer.FourierTransform();
		RCTOCREC(ReconTimer,ReconS_21);
		RCTICREC(ReconTimer,ReconS_22);
	    FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
	    {
	    	long int idx = ROUND(sqrt(kp*kp + ip*ip + jp*jp));
	    	spectrum(idx) += norm(dAkij(Fconv, k, i, j));
	        count(idx) += 1.;
	    }
	    spectrum /= count;

		// Factor two because of two-dimensionality of the complex plane
		// (just like sigma2_noise estimates, the power spectra should be divided by 2)
		RFLOAT normfft = (ref_dim == 3 && data_dim == 2) ? (RFLOAT)(ori_size * ori_size) : 1.;
		spectrum *= normfft / 2.;

		// New SNR^MAP will be power spectrum divided by the noise in the reconstruction (i.e. sigma2)
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(data_vs_prior)
		{
			DIRECT_MULTIDIM_ELEM(tau2, n) =  tau2_fudge * DIRECT_MULTIDIM_ELEM(spectrum, n);
		}
		RCTOCREC(ReconTimer,ReconS_22);
	}
	RCTICREC(ReconTimer,ReconS_23);
	// Completely empty the transformer object
	transformer.cleanup();
    // Now can use extra mem to move data into smaller array space
    vol_out.shrinkToFit();

	RCTOCREC(ReconTimer,ReconS_23);
#ifdef TIMINGREC
    if(printTimes)
    	ReconTimer.printTimes(true);
#endif


#ifdef DEBUG_RECONSTRUCT
    std::cerr<<"done with reconstruct"<<std::endl;
#endif

	tau2_io = tau2;
    sigma2_out = sigma2;
    data_vs_prior_out = data_vs_prior;
    fourier_coverage_out = fourier_coverage;
}

void BackProjector::reconstruct_gpu_transpose_test(MultidimArray<RFLOAT> &vol_out,
                                int max_iter_preweight,
                                bool do_map,
                                RFLOAT tau2_fudge,
                                MultidimArray<RFLOAT> &tau2_io, // can be input/output
                                MultidimArray<RFLOAT> &sigma2_out,
                                MultidimArray<RFLOAT> &data_vs_prior_out,
                                MultidimArray<RFLOAT> &fourier_coverage_out,
                                const MultidimArray<RFLOAT> &fsc, // only input
                                RFLOAT normalise,
                                bool update_tau2_with_fsc,
                                bool is_whole_instead_of_half,
                                int nr_threads,
                                int minres_map,
                                bool printTimes,
								bool do_fsc0999)
{

#ifdef TIMINGREC
	Timer ReconTimer;
	printTimes=1;
	int ReconS_1 = ReconTimer.setNew(" RcS1_Init ");
	int ReconS_2 = ReconTimer.setNew(" RcS2_Shape&Noise ");
	int ReconS_2_5 = ReconTimer.setNew(" RcS2.5_Regularize ");
	int ReconS_3 = ReconTimer.setNew(" RcS3_skipGridding ");
	int ReconS_4 = ReconTimer.setNew(" RcS4_doGridding_norm ");
	int ReconS_5 = ReconTimer.setNew(" RcS5_doGridding_init ");
	int ReconS_6 = ReconTimer.setNew(" RcS6_doGridding_iter ");
	int ReconS_7 = ReconTimer.setNew(" RcS7_doGridding_apply ");
	int ReconS_8 = ReconTimer.setNew(" RcS8_blobConvolute ");
	int ReconS_9 = ReconTimer.setNew(" RcS9_blobResize ");
	int ReconS_10 = ReconTimer.setNew(" RcS10_blobSetReal ");
	int ReconS_11 = ReconTimer.setNew(" RcS11_blobSetTemp ");
	int ReconS_12 = ReconTimer.setNew(" RcS12_blobTransform ");
	int ReconS_13 = ReconTimer.setNew(" RcS13_blobCenterFFT ");
	int ReconS_14 = ReconTimer.setNew(" RcS14_blobNorm1 ");
	int ReconS_15 = ReconTimer.setNew(" RcS15_blobSoftMask ");
	int ReconS_16 = ReconTimer.setNew(" RcS16_blobNorm2 ");
	int ReconS_17 = ReconTimer.setNew(" RcS17_WindowReal ");
	int ReconS_18 = ReconTimer.setNew(" RcS18_GriddingCorrect ");
	int ReconS_19 = ReconTimer.setNew(" RcS19_tauInit ");
	int ReconS_20 = ReconTimer.setNew(" RcS20_tausetReal ");
	int ReconS_21 = ReconTimer.setNew(" RcS21_tauTransform ");
	int ReconS_22 = ReconTimer.setNew(" RcS22_tautauRest ");
	int ReconS_23 = ReconTimer.setNew(" RcS23_tauShrinkToFit ");
	int ReconS_24 = ReconTimer.setNew(" RcS24_extra ");
#endif

    // never rely on references (handed to you from the outside) for computation:
    // they could be the same (i.e. reconstruct(..., dummy, dummy, dummy, dummy, ...); )

    MultidimArray<RFLOAT> sigma2, data_vs_prior, fourier_coverage;
	MultidimArray<RFLOAT> tau2 = tau2_io;


    RCTICREC(ReconTimer,ReconS_1);
    FourierTransformer transformer;
	MultidimArray<RFLOAT> Fweight;
	// Fnewweight can become too large for a float: always keep this one in double-precision
	MultidimArray<double> Fnewweight;
	MultidimArray<Complex>& Fconv = transformer.getFourierReference();
	int max_r2 = ROUND(r_max * padding_factor) * ROUND(r_max * padding_factor);

//#define DEBUG_RECONSTRUCT
#ifdef DEBUG_RECONSTRUCT
	Image<RFLOAT> ttt;
	FileName fnttt;
	ttt()=weight;
	ttt.write("reconstruct_initial_weight.spi");
	std::cerr << " pad_size= " << pad_size << " padding_factor= " << padding_factor << " max_r2= " << max_r2 << std::endl;
#endif

    // Set Fweight, Fnewweight and Fconv to the right size
    if (ref_dim == 2)
        vol_out.setDimensions(pad_size, pad_size, 1, 1);
    else
        // Too costly to actually allocate the space
        // Trick transformer with the right dimensions
        vol_out.setDimensions(pad_size, pad_size, pad_size, 1);

    transformer.setReal(vol_out); // Fake set real. 1. Allocate space for Fconv 2. calculate plans.
    vol_out.clear(); // Reset dimensions to 0

    RCTOCREC(ReconTimer,ReconS_1);
    RCTICREC(ReconTimer,ReconS_2);

    Fweight.reshape(Fconv);
    if (!skip_gridding)
    	Fnewweight.reshape(Fconv);

	// Go from projector-centered to FFTW-uncentered
	decenter(weight, Fweight, max_r2);

	// Take oversampling into account
	RFLOAT oversampling_correction = (ref_dim == 3) ? (padding_factor * padding_factor * padding_factor) : (padding_factor * padding_factor);
	MultidimArray<RFLOAT> counter;

	// First calculate the radial average of the (inverse of the) power of the noise in the reconstruction
	// This is the left-hand side term in the nominator of the Wiener-filter-like update formula
	// and it is stored inside the weight vector
	// Then, if (do_map) add the inverse of tau2-spectrum values to the weight
	sigma2.initZeros(ori_size/2 + 1);
	counter.initZeros(ori_size/2 + 1);
	FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
	{
		int r2 = kp * kp + ip * ip + jp * jp;
		if (r2 < max_r2)
		{
			int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
			RFLOAT invw = oversampling_correction * DIRECT_A3D_ELEM(Fweight, k, i, j);
			DIRECT_A1D_ELEM(sigma2, ires) += invw;
			DIRECT_A1D_ELEM(counter, ires) += 1.;
		}
    }

	// Average (inverse of) sigma2 in reconstruction
	FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(sigma2)
	{
        if (DIRECT_A1D_ELEM(sigma2, i) > 1e-10)
            DIRECT_A1D_ELEM(sigma2, i) = DIRECT_A1D_ELEM(counter, i) / DIRECT_A1D_ELEM(sigma2, i);
        else if (DIRECT_A1D_ELEM(sigma2, i) == 0)
            DIRECT_A1D_ELEM(sigma2, i) = 0.;
		else
		{
			std::cerr << " DIRECT_A1D_ELEM(sigma2, i)= " << DIRECT_A1D_ELEM(sigma2, i) << std::endl;
			REPORT_ERROR("BackProjector::reconstruct: ERROR: unexpectedly small, yet non-zero sigma2 value, this should not happen...a");
        }
    }

	if (update_tau2_with_fsc)
    {
        tau2.reshape(ori_size/2 + 1);
        data_vs_prior.initZeros(ori_size/2 + 1);
		// Then calculate new tau2 values, based on the FSC
		if (!fsc.sameShape(sigma2) || !fsc.sameShape(tau2))
		{
			fsc.printShape(std::cerr);
			tau2.printShape(std::cerr);
			sigma2.printShape(std::cerr);
			REPORT_ERROR("ERROR BackProjector::reconstruct: sigma2, tau2 and fsc have different sizes");
		}
		FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(sigma2)
        {
			// FSC cannot be negative or zero for conversion into tau2
			RFLOAT myfsc = XMIPP_MAX(0.001, DIRECT_A1D_ELEM(fsc, i));
			if (is_whole_instead_of_half)
			{
				// Factor two because of twice as many particles
				// Sqrt-term to get 60-degree phase errors....
				myfsc = sqrt(2. * myfsc / (myfsc + 1.));
			}
			myfsc = XMIPP_MIN(0.999, myfsc);
			RFLOAT myssnr = myfsc / (1. - myfsc);
			// Sjors 29nov2017 try tau2_fudge for pulling harder on Refine3D runs...
            myssnr *= tau2_fudge;
			RFLOAT fsc_based_tau = myssnr * DIRECT_A1D_ELEM(sigma2, i);
			DIRECT_A1D_ELEM(tau2, i) = fsc_based_tau;
			// data_vs_prior is merely for reporting: it is not used for anything in the reconstruction
			DIRECT_A1D_ELEM(data_vs_prior, i) = myssnr;
		}
	}
    RCTOCREC(ReconTimer,ReconS_2);
    RCTICREC(ReconTimer,ReconS_2_5);
	// Apply MAP-additional term to the Fnewweight array
	// This will regularise the actual reconstruction
    if (do_map)
	{

    	// Then, add the inverse of tau2-spectrum values to the weight
		// and also calculate spherical average of data_vs_prior ratios
		if (!update_tau2_with_fsc)
			data_vs_prior.initZeros(ori_size/2 + 1);
		fourier_coverage.initZeros(ori_size/2 + 1);
		counter.initZeros(ori_size/2 + 1);
		FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
 		{
			int r2 = kp * kp + ip * ip + jp * jp;
			if (r2 < max_r2)
			{
				int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
				RFLOAT invw = DIRECT_A3D_ELEM(Fweight, k, i, j);

				RFLOAT invtau2;
				if (DIRECT_A1D_ELEM(tau2, ires) > 0.)
				{
					// Calculate inverse of tau2
					invtau2 = 1. / (oversampling_correction * tau2_fudge * DIRECT_A1D_ELEM(tau2, ires));
				}
				else if (DIRECT_A1D_ELEM(tau2, ires) == 0.)
				{
					// If tau2 is zero, use small value instead
					invtau2 = 1./ ( 0.001 * invw);
				}
				else
				{
					std::cerr << " sigma2= " << sigma2 << std::endl;
					std::cerr << " fsc= " << fsc << std::endl;
					std::cerr << " tau2= " << tau2 << std::endl;
					REPORT_ERROR("ERROR BackProjector::reconstruct: Negative or zero values encountered for tau2 spectrum!");
				}

				// Keep track of spectral evidence-to-prior ratio and remaining noise in the reconstruction
				if (!update_tau2_with_fsc)
					DIRECT_A1D_ELEM(data_vs_prior, ires) += invw / invtau2;

				// Keep track of the coverage in Fourier space
				if (invw / invtau2 >= 1.)
					DIRECT_A1D_ELEM(fourier_coverage, ires) += 1.;

				DIRECT_A1D_ELEM(counter, ires) += 1.;

				// Only for (ires >= minres_map) add Wiener-filter like term
				if (ires >= minres_map)
				{
					// Now add the inverse-of-tau2_class term
					invw += invtau2;
					// Store the new weight again in Fweight
					DIRECT_A3D_ELEM(Fweight, k, i, j) = invw;
				}
			}
		}

		// Average data_vs_prior
		if (!update_tau2_with_fsc)
		{
			FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(data_vs_prior)
			{
				if (i > r_max)
					DIRECT_A1D_ELEM(data_vs_prior, i) = 0.;
				else if (DIRECT_A1D_ELEM(counter, i) < 0.001)
					DIRECT_A1D_ELEM(data_vs_prior, i) = 999.;
				else
					DIRECT_A1D_ELEM(data_vs_prior, i) /= DIRECT_A1D_ELEM(counter, i);
			}
		}

		// Calculate Fourier coverage in each shell
		FOR_ALL_DIRECT_ELEMENTS_IN_ARRAY1D(fourier_coverage)
		{
			if (DIRECT_A1D_ELEM(counter, i) > 0.)
				DIRECT_A1D_ELEM(fourier_coverage, i) /= DIRECT_A1D_ELEM(counter, i);
		}

	} //end if do_map
    else if (do_fsc0999)
    {

     	// Sjors 9may2018: avoid numerical instabilities with unregularised reconstructions....
        FOR_ALL_ELEMENTS_IN_FFTW_TRANSFORM(Fconv)
        {
            int r2 = kp * kp + ip * ip + jp * jp;
            if (r2 < max_r2)
            {
                int ires = ROUND( sqrt((RFLOAT)r2) / padding_factor );
                if (ires >= minres_map)
                {
                    // add 1/1000th of the radially averaged sigma2 to the Fweight, to avoid having zeros there...
                	DIRECT_A3D_ELEM(Fweight, k, i, j) += 1./(999. * DIRECT_A1D_ELEM(sigma2, ires));
                }
            }
        }

    }

//==============================================================================add  GPU version

	int Ndim[3];
	int devCount;
	cudaGetDeviceCount(&devCount);
	int GPU_N=devCount;

	Ndim[0]=pad_size;
	Ndim[1]=pad_size;
	Ndim[2]=pad_size;
	size_t fullsize= pad_size*pad_size*pad_size;
    initgpu(GPU_N);

	int offset;
	//divide
	int *numberZ = (int *)malloc(sizeof(int)*GPU_N);
	int *offsetZ = (int *)malloc(sizeof(int)*GPU_N);
	dividetask(numberZ,offsetZ,pad_size,GPU_N);

	MultiGPUplan *plan;
	plan = (MultiGPUplan *)malloc(sizeof(MultiGPUplan)*GPU_N);
	multi_plan_init_transpose(plan,GPU_N,numberZ,offsetZ,pad_size);



	for (int i = 0; i < GPU_N; ++i) {
		cudaSetDevice(plan[i].devicenum);
		cudaMalloc((void**) & (plan[i].d_Data),sizeof(cufftComplex) * plan[i].datasize);
		cudaMalloc((void**) & (plan[i].temp_Data), sizeof(cufftComplex) * plan[i].tempsize);
	}

	//=================================
	int xyN[2];
	xyN[0] = Ndim[0];
	xyN[1] = Ndim[1];
	cufftHandle *xyplan;
	xyplan = (cufftHandle*) malloc(sizeof(cufftHandle)*GPU_N);
	cufftHandle *zplan;
	zplan = (cufftHandle*) malloc(sizeof(cufftHandle)*GPU_N);


    RCTOCREC(ReconTimer,ReconS_2_5);
	if (skip_gridding)
	{
	    RCTICREC(ReconTimer,ReconS_3);
		std::cerr << "Skipping gridding!" << std::endl;
		Fconv.initZeros(); // to remove any stuff from the input volume
		decenter(data, Fconv, max_r2);

		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			if (DIRECT_MULTIDIM_ELEM(Fweight, n) > 0.)
				DIRECT_MULTIDIM_ELEM(Fconv, n) /= DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		RCTOCREC(ReconTimer,ReconS_3);
#ifdef DEBUG_RECONSTRUCT
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			DIRECT_MULTIDIM_ELEM(ttt(), n) = DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		ttt.write("reconstruct_skipgridding_correction_term.spi");
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fconv)
		{
			if (DIRECT_MULTIDIM_ELEM(Fweight, n) > 0.)
				DIRECT_MULTIDIM_ELEM(ttt(), n) = 1./DIRECT_MULTIDIM_ELEM(Fweight, n);
		}
		ttt.write("reconstruct_skipgridding_correction_term_inverse.spi");
#endif
	}
	else
	{
		RCTICREC(ReconTimer,ReconS_4);
		// Divide both data and Fweight by normalisation factor to prevent FFT's with very large values....
	#ifdef DEBUG_RECONSTRUCT
		std::cerr << " normalise= " << normalise << std::endl;
	#endif
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(Fweight)
		{
			DIRECT_MULTIDIM_ELEM(Fweight, n) /= normalise;
		}
		FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(data)
		{
			DIRECT_MULTIDIM_ELEM(data, n) /= normalise;
		}
		RCTOCREC(ReconTimer,ReconS_4);
		RCTICREC(ReconTimer,ReconS_5);
        // Initialise Fnewweight with 1's and 0's. (also see comments below)
		FOR_ALL_ELEMENTS_IN_ARRAY3D(weight)
		{
			if (k * k + i * i + j * j < max_r2)
				A3D_ELEM(weight, k, i, j) = 1.;
			else
				A3D_ELEM(weight, k, i, j) = 0.;
		}



		decenter(weight, Fnewweight, max_r2);
		RCTOCREC(ReconTimer,ReconS_5);
		// Iterative algorithm as in  Eq. [14] in Pipe & Menon (1999)
		// or Eq. (4) in Matej (2001)


		long int Fconvnum=Fconv.nzyxdim;
		long int normsize= Ndim[0]*Ndim[1]*Ndim[2];
		int halfxdim=Fconv.xdim;

		printf("%ld %ld %ld \n",Fconv.xdim,Fconv.ydim,Fconv.zdim);
		printf("Fnewweight: %ld %ld %ld \n",Fnewweight.xdim,Fnewweight.ydim,Fnewweight.zdim);
		multi_enable_access(plan,GPU_N);


         //==================2d fft plan

		for (int i = 0; i < GPU_N; i++) {
		    cudaSetDevice(plan[i].devicenum);
			cufftPlanMany(&xyplan[i], 2, xyN, NULL, 0, 0, NULL, 0, 0, CUFFT_C2C, numberZ[i]);
		}
	    //==================1d fft plan

		for (int i = 0; i < GPU_N; i++) {
		    cudaSetDevice(plan[i].devicenum);
			int rank=1;
			int nrank[1];
			nrank[0]=Ndim[1];
			int inembed[2];
			inembed[0]=Ndim[0];
			inembed[1]=Ndim[1];
			cufftPlanMany(&zplan[i], rank, nrank, inembed, Ndim[0] , 1, inembed, Ndim[0], 1, CUFFT_C2C, Ndim[0]);
		}


		float *fweightdata= (float *)malloc(sizeof(float)*pad_size*pad_size*pad_size);
		layoutchange(Fweight.data,Fweight.xdim,Fweight.ydim,Fweight.zdim,pad_size,fweightdata);
		float **d_blocktwo;
		d_blocktwo =(float **)malloc(GPU_N*sizeof(float*));
		for(int i=0;i< GPU_N;i++)
		{
			cudaSetDevice(plan[i].devicenum);
			cudaMalloc((void**) &(d_blocktwo[i]),sizeof(float) * plan[i].realsize);
			cudaMemcpy(d_blocktwo[i],fweightdata+plan[i].selfoffset,plan[i].realsize*sizeof(float),cudaMemcpyHostToDevice);
		}
		multi_sync(plan,GPU_N);
		free(fweightdata);


		double *tempdata= (double *)malloc(sizeof(double)*pad_size*pad_size*pad_size);
		// every thread map two block to used
		layoutchange(Fnewweight.data,Fnewweight.xdim,Fnewweight.ydim,Fnewweight.zdim,pad_size,tempdata);
		double **d_blockone;
		d_blockone = (double **)malloc(GPU_N*sizeof(double *));

		for (int i = 0; i < GPU_N; i++) {
		    cudaSetDevice(plan[i].devicenum);
			cudaMalloc((void**) &(d_blockone[i]),sizeof(double) * plan[i].realsize);
			cudaMemcpy(d_blockone[i],tempdata+plan[i].selfoffset,plan[i].realsize*sizeof(double),cudaMemcpyHostToDevice);
		}
		multi_sync(plan,GPU_N);


		cufftComplex *cpu_data=(cufftComplex* )malloc(sizeof(cufftComplex)*pad_size*pad_size*pad_size);
		for(int i=0;i<fullsize;i++)
		{
			cpu_data[i].x=i;
			cpu_data[i].y=0;
		}



		for(int i = 0; i < GPU_N; i++)
		{
			 cudaSetDevice(plan[i].devicenum);
			 //int reali= i+ranknum*GPU_N;
			 cudaMemcpy(plan[i].d_Data, cpu_data + pad_size*pad_size*offsetZ[i], pad_size*pad_size*numberZ[i]*sizeof(cufftComplex),cudaMemcpyHostToDevice);
		}







		for (int iter = 0; iter < max_iter_preweight; iter++)
		{

            //std::cout << "    iteration " << (iter+1) << "/" << max_iter_preweight << "\n";
			RCTICREC(ReconTimer,ReconS_6);

			cudaMemcpy(cpu_data,plan[0].d_Data,pad_size*pad_size*sizeof(cufftComplex),cudaMemcpyDeviceToHost);
			printf("1\n");
			for(int i=0;i<pad_size;i++)
				//for(int j=0;j<pad_size;j++)
				printf("%f ",cpu_data[i*pad_size].x);
			printf("\n");


			//yzlocal_transpose(plan,GPU_N,pad_size,numberZ,offsetZ,offsettmpZ);
			//transpose_exchange(plan,GPU_N,pad_size,offsetZ);


			cudaMemcpy(cpu_data,plan[0].d_Data,pad_size*pad_size*sizeof(cufftComplex),cudaMemcpyDeviceToHost);
			printf("2\n");
			for(int i=0;i<pad_size;i++)
				//for(int j=0;j<pad_size;j++)
				printf("%f ",cpu_data[i*pad_size].x);
			printf("\n");


		}


	} // end if skip_gridding


}
