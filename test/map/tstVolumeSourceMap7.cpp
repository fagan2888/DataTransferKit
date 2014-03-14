//---------------------------------------------------------------------------//
/*!
 * \file tstVolumeSourceMap6.cpp
 * \author Stuart R. Slattery
 * \brief Volume source map unit test 6 for repeated geometry transfer.
 */
//---------------------------------------------------------------------------//

#include <iostream>
#include <vector>
#include <map>
#include <limits>
#include <cmath>
#include <sstream>
#include <algorithm>
#include <cassert>
#include <ctime>
#include <cstdlib>

#include <DTK_VolumeSourceMap.hpp>
#include <DTK_FieldTraits.hpp>
#include <DTK_FieldEvaluator.hpp>
#include <DTK_FieldManager.hpp>
#include <DTK_FieldTools.hpp>
#include <DTK_FieldContainer.hpp>
#include <DTK_GeometryTraits.hpp>
#include <DTK_GeometryManager.hpp>
#include <DTK_Cylinder.hpp>
#include <DTK_Box.hpp>

#include <Teuchos_UnitTestHarness.hpp>
#include <Teuchos_DefaultComm.hpp>
#include <Teuchos_CommHelpers.hpp>
#include <Teuchos_RCP.hpp>
#include <Teuchos_ArrayRCP.hpp>
#include <Teuchos_Ptr.hpp>
#include <Teuchos_OpaqueWrapper.hpp>
#include <Teuchos_Array.hpp>
#include <Teuchos_ArrayView.hpp>
#include <Teuchos_TypeTraits.hpp>

//---------------------------------------------------------------------------//
// MPI Setup
//---------------------------------------------------------------------------//

template<class Ordinal>
Teuchos::RCP<const Teuchos::Comm<Ordinal> > getDefaultComm()
{
#ifdef HAVE_MPI
    return Teuchos::DefaultComm<Ordinal>::getComm();
#else
    return Teuchos::rcp(new Teuchos::SerialComm<Ordinal>() );
#endif
}

//---------------------------------------------------------------------------//
// FieldEvaluator Implementation.
class MyEvaluator : 
    public DataTransferKit::FieldEvaluator<int,DataTransferKit::FieldContainer<double> >
{
  public:

    MyEvaluator( const Teuchos::ArrayRCP<int>& geom_gids, 
		 const Teuchos::RCP< const Teuchos::Comm<int> >& comm )
	: d_geom_gids( geom_gids )
	, d_comm( comm )
    { /* ... */ }

    ~MyEvaluator()
    { /* ... */ }

    DataTransferKit::FieldContainer<double> evaluate( 
	const Teuchos::ArrayRCP<int>& gids,
	const Teuchos::ArrayRCP<double>& coords )
    {
	Teuchos::ArrayRCP<double> evaluated_data( gids.size() );
	for ( int n = 0; n < gids.size(); ++n )
	{
	    if ( std::find( d_geom_gids.begin(),
			    d_geom_gids.end(),
			    gids[n] ) != d_geom_gids.end() )
	    {
		evaluated_data[n] = 1.0 + gids[n];
	    }
	    else
	    {
		evaluated_data[n] = 0.0;
	    }
	}
	return DataTransferKit::FieldContainer<double>( evaluated_data, 1 );
    }

  private:

    Teuchos::ArrayRCP<int> d_geom_gids;
    Teuchos::RCP< const Teuchos::Comm<int> > d_comm;
};

//---------------------------------------------------------------------------//
// Unit tests. This is a one-to-many transfer.
//---------------------------------------------------------------------------//
TEUCHOS_UNIT_TEST( VolumeSourceMap, cylinder_test )
{
    using namespace DataTransferKit;
    typedef FieldContainer<double> FieldType;

    // Setup communication.
    Teuchos::RCP<const Teuchos::Comm<int> > comm = getDefaultComm<int>();

    // Setup source geometry on proc 0 only.
    int geom_dim = 3;
    int num_geom = 4;
    double length = 2.5;
    double radius = 0.75;
    double center_z = 0.25;
    Teuchos::ArrayRCP<Cylinder> geometry(0);
    Teuchos::ArrayRCP<int> geom_gids(0);
    if ( comm->getRank() == 0 )
    {
	geometry = Teuchos::ArrayRCP<Cylinder>(num_geom);
	geometry[0] = Cylinder( length, radius, -1.5, -1.5, center_z );
	geometry[1] = Cylinder( length, radius,  1.5, -1.5, center_z );
	geometry[2] = Cylinder( length, radius,  1.5,  1.5, center_z );
	geometry[3] = Cylinder( length, radius, -1.5,  1.5, center_z );

	geom_gids = Teuchos::ArrayRCP<int>(num_geom);
	for ( int i = 0; i < num_geom; ++i )
	{
	    geom_gids[i] = i;
	}
    }

    Teuchos::RCP<GeometryManager<Cylinder,int> > source_geometry_manager =
	Teuchos::rcp( new GeometryManager<Cylinder,int>( 
			      geometry, geom_gids, comm, geom_dim ) );

    Teuchos::RCP<FieldEvaluator<int,FieldType> > source_evaluator = 
	Teuchos::rcp( new MyEvaluator( geom_gids, comm ) );

    // Setup target coords on all procs. Add a bogus point.
    Teuchos::ArrayRCP<double> target_coords( (num_geom+1)*geom_dim );
    target_coords[0] = -1.5;
    target_coords[1] = 1.5;
    target_coords[2] = 1.5;
    target_coords[3] = -1.5;
    target_coords[4] = std::numeric_limits<int>::max();
    target_coords[5] = -1.5;
    target_coords[6] = -1.5;
    target_coords[7] = 1.5;
    target_coords[8] = 1.5;
    target_coords[9] = std::numeric_limits<int>::max();
    target_coords[10] = center_z;
    target_coords[11] = center_z;
    target_coords[12] = center_z;
    target_coords[13] = center_z;
    target_coords[14] = std::numeric_limits<int>::max();

    Teuchos::RCP<FieldType > coord_field =
	Teuchos::rcp( new FieldType( target_coords, geom_dim ) );

    Teuchos::RCP<FieldManager<FieldType> > target_coord_manager = 
	Teuchos::rcp( new FieldManager<FieldType>( coord_field, comm ) );

    // Setup target field.
    int target_field_dim = 1;
    Teuchos::ArrayRCP<double> target_data( num_geom+1 );
    Teuchos::RCP<FieldType> target_field =
	Teuchos::rcp( new FieldType( target_data, target_field_dim ) );

    Teuchos::RCP<FieldManager<FieldType> > target_space_manager = 
	Teuchos::rcp( new FieldManager<FieldType>( target_field, comm ) );

    // Setup and apply the volume source mapping.
    VolumeSourceMap<Cylinder,int,FieldType> volume_source_map( 
	comm, geom_dim, true, 1.0e-6 );
    volume_source_map.setup( source_geometry_manager, target_coord_manager );
    volume_source_map.apply( source_evaluator, target_space_manager );

    // Check the evaluation.
    for ( int i = 0; i < num_geom; ++i )
    {
	TEST_EQUALITY( target_data[i], 1.0 + i );
    }
    TEST_EQUALITY( target_data[num_geom], 0.0 );

    // Make sure all points were found except the bogus point.
    TEST_EQUALITY( volume_source_map.getMissedTargetPoints().size(), 1 );
}

//---------------------------------------------------------------------------//
// end tstVolumeSourceMap6.cpp
//---------------------------------------------------------------------------//
