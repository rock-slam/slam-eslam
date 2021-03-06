#include <vizkit3d/QtThreadedWidget.hpp>
#include <vizkit3d/EnvireWidget.hpp>
#include <envire/maps/MLSGrid.hpp>
#include <vizkit3d/AsguardVisualization.hpp>
#include <odometry/ContactOdometry.hpp>
#include <asguard/Configuration.hpp>
#include <eslam/ContactModel.hpp>

#include <Eigen/Geometry>

#include <boost/random/normal_distribution.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/variate_generator.hpp>

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>

#include <boost/math/distributions/normal.hpp>

#include <numeric/Stats.hpp>
#include <numeric/Histogram.hpp>

#include <fstream>

using namespace std;
using namespace envire;
using namespace vizkit3d;

struct Config
{
    double sigma_step, sigma_body, sigma_sensor;
    double sigma_factor;

    size_t max_steps;
    size_t max_runs;
    size_t min_contacts;

    string result_file;

    void set( string const& conf_file )
    {
	ifstream cf( conf_file.c_str() );
	string line;
	map<string,string> conf;
	while( getline( cf, line ) )
	{
	    vector<string> words;
	    boost::split(words, line, boost::is_any_of("="), boost::token_compress_on);
	    if( words.size() == 2 )
	    {
		conf[words[0]] = words[1];
	    }
	}
	sigma_factor = boost::lexical_cast<double>(conf["sigma_factor"]);
	sigma_step = boost::lexical_cast<double>(conf["sigma_step"]);
	sigma_body = boost::lexical_cast<double>(conf["sigma_body"]);
	sigma_sensor = boost::lexical_cast<double>(conf["sigma_sensor"]);
	max_steps = boost::lexical_cast<size_t>(conf["max_steps"]);
	max_runs = boost::lexical_cast<size_t>(conf["max_runs"]);
	min_contacts = boost::lexical_cast<size_t>(conf["min_contacts"]);
	result_file = conf["result_file"];
    }
};

struct AsguardSim
{
    asguard::Configuration asguardConfig;
    asguard::BodyState bodyState;
    odometry::FootContact odometry;
    Eigen::Affine3d body2world;
    odometry::BodyContactState contactState;

    AsguardSim()
	: odometry( odometry::Configuration() )
    {
	body2world = Eigen::Affine3d::Identity();
	for(int j=0;j<4;j++)
	    bodyState.wheelPos[j] = 0.0;
	bodyState.twistAngle = 0;

	// put the robot so that the feet are in 0 height
	body2world.translation().z() = 
	    -asguardConfig.getLowestFootPosition( bodyState ).z();
    }

    void step()
    {
	for( int s=0; s<10; s++ )
	{
	    // odometry udpate
	    for(int j=0;j<4;j++)
		bodyState.wheelPos[j] += 0.01;

	    asguardConfig.setContactState( bodyState, contactState );
	    odometry.update( contactState, Eigen::Quaterniond::Identity() );
	    body2world = body2world * odometry.getPoseDelta().toTransform();
	}
	// odometry seems to get z height wrong in the case of
	// a foot transition
	// TODO investigate and fix
	body2world.translation().z() = 
	    -asguardConfig.getLowestFootPosition( bodyState ).z();
    }
};

struct ContactMeasurementTest
{
    AsguardSim sim;
    boost::variate_generator<boost::mt19937, boost::normal_distribution<> > nrand;
    Config conf;

    base::Histogram contact, nocontact;

    ContactMeasurementTest() :
	nrand( boost::mt19937(time(0)),
		boost::normal_distribution<>()),
	contact( 100, -.1, .5 ),
	nocontact( 100, -.1, .5 )
    {
    }

    void run()
    {
	for(size_t i=0; i<conf.max_steps; i++)
	{
	    step( i );
	}

	double scale = (contact.total() + nocontact.total()) * contact.getBucketWidth();

	std::ofstream fc("contact.dat");
	for( size_t i=0; i<contact.size(); i++)
	    fc << contact.getCenter(i) << " " << contact[i] / scale << std::endl;
	fc.close();

	std::ofstream nc("nocontact.dat");
	for( size_t i=0; i<nocontact.size(); i++)
	    nc << nocontact.getCenter(i) << " " << nocontact[i] / scale << std::endl;;
	nc.close();

	boost::math::normal n( 0, conf.sigma_step );
	std::ofstream r("pdfcdf.dat");
	for( size_t i=0; i<contact.size(); i++)
	{
	    double z = contact.getCenter(i);
	    double model = 
		pdf( n, z ) / cdf( n, z );
	    double ratio = 
		nocontact[i] > 0 ?
		contact[i] / nocontact[i] : std::numeric_limits<double>::quiet_NaN();
		
	    r << z << " " << ratio << " " << model << std::endl;  
	}
	nc.close();


    }

    void step( size_t idx )
    {
	sim.step();

	double z_offset = sim.body2world.translation().z();

	// go through all the feet on one wheel 
	for( size_t i=0; i<asguard::NUMBER_OF_FEET; i++ )
	{
	    double z_pos = 
		sim.asguardConfig.getFootPosition( sim.bodyState, asguard::FRONT_LEFT, i ).z() + z_offset;
	    bool hasContact = fabs( z_pos ) < 1e-3; 
	    z_pos += nrand() * conf.sigma_step;
	    if( hasContact )
		contact.update( z_pos );
	    else
		nocontact.update( z_pos );
	}
    }
};

struct MapTest
{
    Environment *env;
    MLSGrid* grid; 

    AsguardSim sim;
    boost::variate_generator<boost::mt19937, boost::normal_distribution<> > nrand;
    eslam::ContactModel contactModel;

    Config conf;

    double z_var, z_pos;
    double lastY;
    std::vector<double> z_vars;

    MapTest()
	: 
	env(0), grid(0),
	nrand( boost::mt19937(time(0)),
		boost::normal_distribution<>())
    {
    }

    virtual ~MapTest()
    {
    }

    virtual void init()
    {
	if( grid )
	    env->detachItem( grid );

	grid = new MLSGrid( 200, 200, 0.05, 0.05, -5, 0 );
	env->setFrameNode( grid, env->getRootNode() );

	sim = AsguardSim();

	z_pos = sim.body2world.translation().z();
	z_var = 0;
	lastY = 0;

	eslam::ContactModelConfiguration cmconf;
	cmconf.minContacts = conf.min_contacts;
	cmconf.contactLikelihoodCorrection = conf.sigma_factor;
	contactModel.setConfiguration( cmconf );
	z_vars.resize(0);
    }

    virtual void run()
    {
	for(size_t i=0; i<conf.max_steps; i++)
	{
	    step( i );
	}
    }

    MLSGrid::SurfacePatch* getMap( Eigen::Vector3d const& pos )
    {
	MLSGrid::Position pi;
	if( grid->toGrid( (pos).head<2>(), pi ) )
	{
	    // only one patch per cell
	    return grid->get( pi, MLSGrid::SurfacePatch(0,1e9) ); 
	}
	return NULL;
    }

    bool getMap( Eigen::Vector3d const& pos, MLSGrid::SurfacePatch& patch )
    {
	MLSGrid::SurfacePatch *p = getMap( pos );
	if( p )
	{
	    patch = *p;
	    return true;
	}
	
	return false;
    }

    virtual void step( int stepIdx )
    {
	// run simulation and get real z_delta
	double z_delta = sim.body2world.translation().z();
	sim.step();
	z_delta = sim.body2world.translation().z() - z_delta;

	// handle z position uncertainty
	z_pos += z_delta + nrand() * conf.sigma_step;
	z_var += pow(conf.sigma_step,2);

	// our believe of body2world
	Eigen::Affine3d body2world( sim.body2world );
	body2world.translation().z() = z_pos;

	// measurement of the body on the grid
	contactModel.setContactPoints( 
		sim.contactState, 
		Eigen::Quaterniond(body2world.linear()) );

	bool hasContact = contactModel.evaluatePose( 
		Eigen::Affine3d( Eigen::Translation3d( body2world.translation() ) ), 
		pow( conf.sigma_body, 2 ) + z_var, 
		boost::bind( &MapTest::getMap, this, _1, _2 ) );

	double y_pos = sim.body2world.translation().y();
	if( hasContact && (lastY + 0.05) < y_pos )
	{
	    contactModel.updateZPositionEstimate( z_pos, z_var );
	    lastY = y_pos;
	}

	// generate grid cells
	for( size_t i=0; i<50; i++ )
	{
	    // z height of measurement
	    double z_meas = 
		-sim.body2world.translation().z() 
		+ nrand() * conf.sigma_sensor; 

	    Eigen::Vector3d m_pos( 
		    ((float)i-25.0)*0.02, 
		    1.0, 
		    z_meas );
	    m_pos = body2world * m_pos;
	    MLSGrid::Position p;
	    if( grid->toGrid( (m_pos).head<2>(), p ) )
	    {
		double sigma = sqrt( pow(conf.sigma_sensor,2) + z_var );
		// for now, only add new cells
		if( grid->beginCell( p.x, p.y ) == grid->endCell() )
		{
		    MLSGrid::SurfacePatch patch( m_pos.z(), sigma );
		    patch.update_idx = stepIdx;
		    grid->updateCell( 
			p,
			patch );
		}
	    }
	}

	z_vars.push_back( z_var );
    }

};

struct VizMapTest : public MapTest
{
    QtThreadedWidget<envire::EnvireWidget> app;
    AsguardVisualization aviz;

    VizMapTest()
    {
	aviz.setXForward( false );
	app.start();
	app.getWidget()->addPlugin( &aviz );
	env = app.getWidget()->getEnvironment();
    }

    void updateViz()
    {
	// viz update
	aviz.updateData( sim.bodyState );
	aviz.updateTransform( sim.body2world );

	env->itemModified( grid );
    }

    virtual void run()
    {
	for(size_t i=0;i<conf.max_steps && app.isRunning();i++)
	{
	    step( i );
	    usleep(100*1000);
	    updateViz();
	}
    }
};

struct StatMapTest : public MapTest
{
    std::vector<base::Stats<double> > height;
    std::vector<double> forward;
    std::vector<double> z_variance;
    std::vector<base::Stats<double> > map_z;
    std::vector<double> map_stdev;

    std::ofstream out;

    StatMapTest()
    {
	env = new envire::Environment();
    }

    void init()
    {
	MapTest::init();

	height.resize( conf.max_steps );
	forward.resize( conf.max_steps );
	z_variance.resize( conf.max_steps );
	map_z.resize( conf.max_steps );
	map_stdev.resize( conf.max_steps );
    }

    ~StatMapTest()
    {
	delete env;
    }

    virtual void run()
    {
	for( size_t run=0; run<conf.max_runs; run++ )
	{
	    std::cerr << "run " << run << "     \r";
	    for( size_t i=0; i<conf.max_steps; i++ )
	    {
		step(i);

		// store results
		height[i].update( z_pos - sim.body2world.translation().z() );
		forward[i] = sim.body2world.translation().y();
		z_variance[i] = z_var;

		// get map height
		MLSGrid::Position p;
		if( grid->toGrid( (sim.body2world.translation()).head<2>(), p ) )
		{
		    MLSGrid::iterator it = grid->beginCell( p.x, p.y );
		    if( it != grid->endCell() )
		    {
			map_z[i].update( it->mean );
			map_stdev[i] = it->stdev;
		    }
		}
	    }
	    init();
	}

	out.open( conf.result_file.c_str() );
	for( size_t i=0; i<conf.max_steps; i++ )
	{
	    out 
		<< i << " "
		<< forward[i] << " "
		<< height[i].mean() << " "
		<< height[i].stdev() << " "
		<< sqrt(z_variance[i]) << " "
		<< map_z[i].mean() << " "
		<< map_z[i].stdev() << " "
		<< map_stdev[i] << " "
		<< height[i].min() << " "
		<< height[i].max() << " "
		<< std::endl;
	}
    }
};

int main( int argc, char **argv )
{
    MapTest *mt;
    std::string mode = argv[1];
    if( mode == "viz" )
    {
	mt = new VizMapTest;
    }
    else if( mode == "batch" )
    {
	mt = new StatMapTest();
    }
    else if( mode == "contact" )
    {
	ContactMeasurementTest t;
	if( argc >=3 )
	    t.conf.set( argv[2] );
	t.run();
	exit(0);
    }
    else
	throw std::runtime_error("mode needs to be either viz or batch");

    if( argc >=3 )
	mt->conf.set( argv[2] );

    mt->init();
    mt->run();

    delete mt;
}
