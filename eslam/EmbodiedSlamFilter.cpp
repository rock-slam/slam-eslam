#include "EmbodiedSlamFilter.hpp"

#include <envire/maps/MultiLevelSurfaceGrid.hpp>
#include <envire/operators/MLSProjection.hpp>
#include <envire/operators/ScanMeshing.hpp>

#include <envire/tools/Numeric.hpp>

using namespace eslam;
using namespace envire;

EmbodiedSlamFilter::EmbodiedSlamFilter(
	const asguard::Configuration& asguardConfig,
	const asguard::odometry::Configuration& odometryConfig, 
	const eslam::Configuration& eslamConfig )
:   eslamConfig( eslamConfig ),
    asguardConfig( asguardConfig ),
    trans( asguardConfig ),
    odometryConfig( odometryConfig ),
    odometry( odometryConfig, asguardConfig ), 
    filter( odometry, eslamConfig, asguardConfig ), 
    sharedMap(NULL) 
{};

MultiLevelSurfaceGrid* EmbodiedSlamFilter::createGridTemplate( envire::Environment* env )
{
    const double size = 20;
    const double resolution = 0.05;
    envire::MultiLevelSurfaceGrid* gridTemplate = 
	    new envire::MultiLevelSurfaceGrid( size/resolution, size/resolution, resolution, resolution );
    envire::FrameNode *gridNode = new envire::FrameNode( Eigen::Transform3d( Eigen::Translation3d( -size/2.0, -size/2.0, 0 ) ) ); 
    env->addChild( env->getRootNode(), gridNode );
    env->setFrameNode( gridTemplate, gridNode );

    gridTemplate->setHorizontalPatchThickness( 0.1 );
    gridTemplate->setGapSize( 1.50 );

    return gridTemplate;
}

MLSMap* EmbodiedSlamFilter::createMapTemplate( envire::Environment* env )
{
    envire::MultiLevelSurfaceGrid* gridTemplate = 
	createGridTemplate( env );

    envire::MultiLevelSurfaceGrid::SurfacePatch p( 0, 1.0, 0, true );
    const size_t cx = gridTemplate->getWidth() / 2.0;
    const size_t cy = gridTemplate->getHeight() / 2.0;
    for( int x=-20; x<20; x++ )
    {
	for( int y=-20; y<20; y++ )
	{
	    gridTemplate->insertTail( cx + x, cy + y, p );
	}
    }

    MLSMap* mapTemplate = new MLSMap();
    FrameNode *mapNode = new envire::FrameNode(); 
    env->addChild( env->getRootNode(), mapNode );
    env->addChild( mapNode, gridTemplate->getFrameNode() );
    env->setFrameNode( mapTemplate, mapNode );
    mapTemplate->addGrid( gridTemplate );

    return mapTemplate;
}

void EmbodiedSlamFilter::init( envire::Environment* env, const base::Pose& pose, bool useSharedMap )
{
    const double angle = pose.orientation.toRotationMatrix().eulerAngles(2,1,0)[0];
    filter.init(
	    eslamConfig.particleCount, 
	    base::Pose2D(Eigen::Vector2d(pose.position.x(),pose.position.y()),angle), 
	    //base::Pose2D(Eigen::Vector2d(config.filter.initialError,config.filter.initialError),config.filter.initialError),
	    base::Pose2D(Eigen::Vector2d(1e-3,1e-3),1e-3),
	    pose.position.z(),
	    //1.0 // sigma_z
	    1e-3
	    );

    odPose = pose;
    udPose = mapPose = base::Pose( Eigen::Vector3d(1000,0,0), Eigen::Quaterniond::Identity() ); 

    if( useSharedMap )
    {
	// see if there is a MLSGrid in the environment and use that as a sharedmap
	// otherwise create a new map
	std::vector<envire::MultiLevelSurfaceGrid*> grids = env->getItems<envire::MultiLevelSurfaceGrid>();
	if( !grids.empty() )
	{
	    // for now use the first grid found...
	    //sharedMap = grids.front();
	    throw std::runtime_error("feature broken");
	}
	else
	    sharedMap = createMapTemplate( env );
    }

    // either use the shared map to init, or create a grid template for the per particle maps
    if( sharedMap )
	filter.setEnvironment( env, sharedMap, useSharedMap );
    else
	filter.setEnvironment( env, createMapTemplate( env ), useSharedMap );

    // setup environment for converting scans
    scanMap = createGridTemplate( env ); 
    scanFrame = new envire::FrameNode();
    scannerFrame = new envire::FrameNode();
    env->addChild( env->getRootNode(), scanFrame );
    env->addChild( scanFrame, scanMap->getFrameNode() );
    env->addChild( scanFrame, scannerFrame );

    scanNode = new envire::LaserScan();
    env->setFrameNode( scanNode, scannerFrame );
    pcNode = new envire::TriMesh();
    env->setFrameNode( pcNode, scannerFrame );

    smOp = new envire::ScanMeshing();
    env->attachItem( smOp );
    smOp->addInput( scanNode );
    smOp->addOutput( pcNode );

    mlsOp = new envire::MLSProjection();
    env->attachItem( mlsOp );
    mlsOp->addInput( pcNode );
    mlsOp->addOutput( scanMap );
    mlsOp->useUncertainty( true );
}


bool EmbodiedSlamFilter::update( const asguard::BodyState& bs, const Eigen::Quaterniond& orientation, const base::samples::LaserScan& scan )
{
    bool result = update( bs, orientation );

    Eigen::Transform3d pdelta( mapPose.toTransform().inverse() * odPose.toTransform() );
    const double max_angle = eslamConfig.mappingThreshold.angle;
    const double max_dist = eslamConfig.mappingThreshold.distance;
    if( Eigen::AngleAxisd( pdelta.rotation() ).angle() > max_angle || pdelta.translation().norm() > max_dist )
    {
	static size_t update_idx = 0;

	// convert scan object to pointcloud
	scanNode->lines.clear();
	scanNode->addScanLine( 0, scan );
	smOp->updateAll();

	if( sharedMap )
	{
	    scanFrame->setTransform( getCentroid().toTransform() * trans.laser2Body );
	    mlsOp->removeOutputs();
	    mlsOp->addOutput( sharedMap->getActiveGrid().get() );
	    mlsOp->updateAll();
	}
	else
	{
	    // assume a 2 deg rotation error for the laser2Body transform
	    const double scanAngleSigma = 5.0/180.0*M_PI;
	    Eigen::Matrix<double,6,1> lcov;
	    lcov << scanAngleSigma,0,0, 0,0,0;
	    envire::TransformWithUncertainty laser2Body( trans.laser2Body, lcov.cwise().square().asDiagonal());
	    
	    // the covariance for the body to world transform comes from
	    // a 1 deg error for pitch and roll
	    // TODO: actually the errors should be in global frame, and not in body
	    // frame... fix later
	    const double pitchRollSigma = 3.0/180.0*M_PI;
	    Eigen::Matrix<double,6,1> pcov;
	    pcov << pitchRollSigma,pitchRollSigma,0, 0,0,0;
	    envire::TransformWithUncertainty body2World( 
		    Eigen::Transform3d( base::removeYaw(orientation) ), pcov.cwise().square().asDiagonal());

	    scannerFrame->setTransform( body2World * laser2Body );
	    scanMap->clear();
	    mlsOp->updateAll();

	    std::vector<eslam::PoseEstimator::Particle> &particles( getParticles() );
	    for( size_t i=0; i< particles.size(); i++ )
	    {
		eslam::PoseEstimator::Particle &p( particles[i] );
		envire::MLSMap *pmap = p.grid.getMap();
		envire::MultiLevelSurfaceGrid *pgrid = pmap->getActiveGrid().get();

		scanFrame->setTransform( envire::Transform( 
			    Eigen::Translation3d( p.position.x(), p.position.y(), 0 ) *
			    Eigen::AngleAxisd( p.orientation, Eigen::Vector3d::UnitZ() )
			    ) );

		// create a new map every n-steps (needs better criteria)
		if( (update_idx % 50) == 0 )
		{
		    std::cout << "create new map" << std::endl;
		    // we are looking for the transform between the active map,
		    // and the current particle
		    Transform tf = scanFrame->relativeTransform( pgrid->getFrameNode() );
		    envire::MultiLevelSurfaceGrid::Point2D cp = pgrid->getCenterPoint();
		    pmap->createGrid( tf * Eigen::Translation3d( -cp.x(), -cp.y(), 0 ) );
		    pgrid = pmap->getActiveGrid().get();
		}

		Eigen::Transform3d C_s2p = scanMap->getEnvironment()->relativeTransform( scanMap->getFrameNode(), pgrid->getFrameNode() );

		typedef envire::MultiLevelSurfaceGrid::Position position;
		typedef envire::MultiLevelSurfaceGrid::SurfacePatch patch;
		std::set<position> &cells = scanMap->getIndex()->cells;

		// this is a two step process.  first perform the measurement
		// and then merge the (possibly updated measurement) into the
		// map. 
		typedef std::pair<position, patch> pos_patch;
		std::vector<pos_patch> patches;

		double d1=0, d2=0;

		// index the patches 
		for(std::set<position>::iterator it = cells.begin(); it != cells.end(); it++)
		{
		    // get center of cell
		    Eigen::Vector3d pos( Eigen::Vector3d::Zero() );
		    scanMap->fromGrid( it->m, it->n, pos.x(), pos.y() );
		    pos = C_s2p * pos;

		    size_t m, n;
		    if( pgrid->toGrid( pos.x(), pos.y(), m, n ) )
		    {
			position pos(m, n);
			for(envire::MultiLevelSurfaceGrid::iterator cit = scanMap->beginCell(it->m,it->n); cit != scanMap->endCell(); cit++ )
			{
			    patch meas_patch( *cit );
			    meas_patch.mean += p.zPos;
			    meas_patch.stdev = sqrt( sq( meas_patch.stdev ) + sq( p.zSigma ) );
			    meas_patch.update_idx = update_idx;
			    patches.push_back( std::make_pair( pos, meas_patch ) );

			    // find a patch in the target map and see if its relevant for measurement
			    patch *tar_patch = pgrid->get( pos, meas_patch, 0.5 ); 
			    if( tar_patch && tar_patch->horizontal && meas_patch.horizontal && (tar_patch->update_idx + 0 < meas_patch.update_idx ) && false )
			    {
				const double diff = meas_patch.mean - tar_patch->mean;
				const double var = sq( tar_patch->stdev ) + sq( meas_patch.stdev );
				d1 += diff / var;
				d2 += 1.0 / var;
			    }
			}
		    }
		}
		double delta = p.zPos;
		if( d2 > 0 )
		{
		    const double mean = d1 / d2;
		    const double var = 1.0 / d2;

		    kalman_update( p.zPos, p.zSigma, p.zPos+mean, var );
		}
		delta = p.zPos - delta;

		// merge the measurement
		for( std::vector<pos_patch>::iterator it = patches.begin();
			it != patches.end(); it++)
		{
		    const position &pos( it->first );
		    patch &pa( it->second );

		    // apply the measurement difference
		    pa.mean += delta;

		    pgrid->updateCell( pos.m, pos.n, pa );
		}
	    }
	}

	update_idx++;
	mapPose = odPose;
    }
    return result;
}

bool EmbodiedSlamFilter::update( const asguard::BodyState& bs, const Eigen::Quaterniond& orientation )
{
    odPose = base::Pose( odPose.toTransform() * odometry.getPoseDelta().toTransform() );

    odometry.update( bs, orientation );
    filter.project( bs, orientation );

    Eigen::Transform3d pdelta( udPose.toTransform().inverse() * odPose.toTransform() );
    const double max_angle = eslamConfig.measurementThreshold.angle;
    const double max_dist = eslamConfig.measurementThreshold.distance;
    if( Eigen::AngleAxisd( pdelta.rotation() ).angle() > max_angle || pdelta.translation().norm() > max_dist )
    {
	filter.update( bs, orientation );
	udPose = odPose;

	return true;
    }
    else
	return false;
}

std::vector<eslam::PoseEstimator::Particle>& EmbodiedSlamFilter::getParticles()
{
    return filter.getParticles();
}

base::Pose EmbodiedSlamFilter::getCentroid()
{
    return filter.getCentroid();
}

base::Pose EmbodiedSlamFilter::getOdometryPose()
{
    return odPose;
}

