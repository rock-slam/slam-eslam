#ifndef __ESLAM_CONFIGURATION_HPP__
#define __ESLAM_CONFIGURATION_HPP__

#include <cmath>

namespace eslam 
{

struct UpdateThreshold
{
    UpdateThreshold( double distance, double angle )
	: distance( distance ), angle( angle ) {};

    double distance;
    double angle;
};

struct Configuration
{
    Configuration() :
	seed( 42u ),
	particleCount( 250 ), 
	minEffective( 50 ), 
	initialError(0.1), 
	weightingFactor( 0.1 ),
	measurementError( 0.1 ),
	discountFactor( 0.9 ),
	measurementThreshold( 0.1, 10*M_PI/180.0 ),
	mappingThreshold( 0.02, 5*M_PI/180.0 )
    {};

    unsigned long seed;
    size_t particleCount;
    size_t minEffective;
    double initialError;
    double weightingFactor;
    double measurementError;
    double discountFactor;
    UpdateThreshold measurementThreshold;
    UpdateThreshold mappingThreshold;
};

}
#endif
