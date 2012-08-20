//---------------------------------------------------------------------------//
/*
  Copyright (c) 2012, Stuart R. Slattery
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  *: Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  *: Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

  *: Neither the name of the University of Wisconsin - Madison nor the
  names of its contributors may be used to endorse or promote products
  derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
//---------------------------------------------------------------------------//
/*!
 * \file DTK_SharedDomainMap.hpp
 * \author Stuart R. Slattery
 * \brief Shared domain map definition.
 */
//---------------------------------------------------------------------------//

#ifndef DTK_SHAREDDOMAINMAP_DEF_HPP
#define DTK_SHAREDDOMAINMAP_DEF_HPP

#include <algorithm>

#include "DTK_FieldTools.hpp"
#include "DTK_Assertion.hpp"
#include "DTK_Rendezvous.hpp"
#include "DTK_MeshTools.hpp"
#include "DTK_BoundingBox.hpp"
#include "DTK_CommIndexer.hpp"

#include <Teuchos_CommHelpers.hpp>
#include <Teuchos_ScalarTraits.hpp>
#include <Teuchos_as.hpp>
#include <Teuchos_Ptr.hpp>

#include <Tpetra_Import.hpp>
#include <Tpetra_Distributor.hpp>
#include <Tpetra_MultiVector.hpp>

namespace DataTransferKit
{
//---------------------------------------------------------------------------//
/*!
 * \brief Constructor.
 *
 * \param comm The communicator over which the map is generated.
 *
 * \param dimension The dimension of the map. This should be consistent with
 * all source and target objects (i.e. only 3 dimensional coordinates will be
 * accepted with a 3 dimensional map). We need this here so we have a global
 * baseline for all objects that may or may not exist on all processes.
 *
 * \param store_missed_points Set to true if it is desired to keep track of
 * the local target points missed during map generation. The default value is
 * false. 
 */
template<class Mesh, class CoordinateField>
SharedDomainMap<Mesh,CoordinateField>::SharedDomainMap( 
    const RCP_Comm& comm, const int dimension, bool store_missed_points )
    : d_comm( comm )
    , d_dimension( dimension )
    , d_store_missed_points( store_missed_points )
{ /* ... */ }

//---------------------------------------------------------------------------//
/*!
 * \brief Destructor.
 */
template<class Mesh, class CoordinateField>
SharedDomainMap<Mesh,CoordinateField>::~SharedDomainMap()
{ /* ... */ }

//---------------------------------------------------------------------------//
/*!
 * \brief Generate the shared domain map.
 *
 * \param source_mesh_manager Source mesh in the shared domain problem. A null
 * RCP is a valid argument. This will be the case when a mesh manager is only
 * constructed on a subset of the processes that the shared domain map is
 * constructed over. Note that the source mesh must exist only on processes
 * that reside within the SharedDomainMap communicator.
 *
 * \param target_coord_manager Target coordinates in the shared domain
 * problem. A null RCP is a valid argument. This will be the case when a field
 * manager is only constructed on a subset of the processes that the shared
 * domain map is constructed over. Note that the target coordinates must exist
 * only on processes that reside within the SharedDomainMap communicator.
 */
template<class Mesh, class CoordinateField>
void SharedDomainMap<Mesh,CoordinateField>::setup( 
    const RCP_MeshManager& source_mesh_manager, 
    const RCP_CoordFieldManager& target_coord_manager )
{
    // Create existence values for the managers.
    bool source_exists = true;
    if ( source_mesh_manager.is_null() ) source_exists = false;
    bool target_exists = true;
    if ( target_coord_manager.is_null() ) target_exists = false;
    d_comm->barrier();

    // Create local to global process indexers for the managers.
    RCP_Comm source_comm;
    if ( source_exists )
    {
	source_comm = source_mesh_manager->comm();
    }
    RCP_Comm target_comm;
    if ( target_exists )
    {
	target_comm = target_coord_manager->comm();
    }
    d_comm->barrier();
    CommIndexer source_indexer( d_comm, source_comm );
    CommIndexer target_indexer( d_comm, target_comm );

    // Check the source and target dimensions for consistency.
    if ( source_exists )
    {
	testPrecondition( source_mesh_manager->dim() == d_dimension );
    }
    d_comm->barrier();

    if ( target_exists )
    {
	testPrecondition( CFT::dim( *target_coord_manager->field() ) 
			  == d_dimension );
    }
    d_comm->barrier();

    // Compute a unique global ordinal for each point in the coordinate field.
    Teuchos::Array<GlobalOrdinal> target_ordinals;
    computePointOrdinals( target_coord_manager, target_ordinals );

    // Build the data import map from the point global ordinals.
    Teuchos::ArrayView<const GlobalOrdinal> import_ordinal_view =
	target_ordinals();
    d_target_map = Tpetra::createNonContigMap<GlobalOrdinal>(
	import_ordinal_view, d_comm );
    testPostcondition( d_target_map != Teuchos::null );

    // Get the global bounding box for the mesh.
    BoundingBox source_box;
    if ( source_exists )
    {
	source_box = source_mesh_manager->globalBoundingBox();
    }
    d_comm->barrier();
    Teuchos::broadcast<int,BoundingBox>(
	*d_comm, source_indexer.l2g(0), 
	Teuchos::Ptr<BoundingBox>(&source_box) );

    // Get the global bounding box for the coordinate field.
    BoundingBox target_box;
    if ( target_exists )
    {
	target_box = FieldTools<CoordinateField>::coordGlobalBoundingBox(
	    *target_coord_manager->field(), target_coord_manager->comm() );
    }
    d_comm->barrier();
    Teuchos::broadcast<int,BoundingBox>( 
	*d_comm, target_indexer.l2g(0), 
	Teuchos::Ptr<BoundingBox>(&target_box) );

    // Intersect the boxes to get the shared domain bounding box.
    BoundingBox shared_domain_box;
    bool has_intersect = BoundingBox::intersectBoxes( source_box, target_box, 
						      shared_domain_box );
    testAssertion( has_intersect );

    // Build a rendezvous decomposition with the source mesh.
    Rendezvous<Mesh> rendezvous( d_comm, d_dimension, shared_domain_box );
    rendezvous.build( source_mesh_manager );

    // Determine the rendezvous destination proc of each point in the
    // coordinate field.
    Teuchos::ArrayRCP<double> coords_view(0,0.0);
    int coord_dim;
    if ( target_exists )
    {
	coord_dim = CFT::dim( *target_coord_manager->field() );
	coords_view = FieldTools<CoordinateField>::nonConstView( 
	    *target_coord_manager->field() );
    }
    d_comm->barrier();
    Teuchos::broadcast<int,int>( 
	*d_comm, target_indexer.l2g(0), Teuchos::Ptr<int>(&coord_dim) );

    Teuchos::Array<int> rendezvous_procs = 
	rendezvous.procsContainingPoints( coords_view );

    // Get the target points that are in the box in which the rendezvous
    // decomposition was generated. The rendezvous algorithm will expand the
    // box slightly based on mesh parameters.
    Teuchos::Array<GlobalOrdinal> targets_in_box;
    if ( target_exists )
    {
	getTargetPointsInBox( rendezvous.getBox(), *target_coord_manager->field(),
			      target_ordinals, targets_in_box );
    }
    d_comm->barrier();

    // Extract those target points that are not in the box. We don't want to
    // send these to the rendezvous decomposition.
    Teuchos::Array<GlobalOrdinal> not_in_box;
    typename Teuchos::Array<GlobalOrdinal>::const_iterator in_box_iterator;
    typename Teuchos::Array<GlobalOrdinal>::const_iterator in_box_begin =
	targets_in_box.begin();
    for ( in_box_iterator = targets_in_box.begin();
	  in_box_iterator != targets_in_box.end();
	  ++in_box_iterator )
    {
	if ( *in_box_iterator == -1 )
	{
	    not_in_box.push_back( 
		std::distance( in_box_begin, in_box_iterator ) );
	}
    }
    std::reverse( not_in_box.begin(), not_in_box.end() );

    typename Teuchos::Array<GlobalOrdinal>::const_iterator not_in_box_iterator;
    for ( not_in_box_iterator = not_in_box.begin();
	  not_in_box_iterator != not_in_box.end();
	  ++not_in_box_iterator )
    {
	rendezvous_procs.remove( *not_in_box_iterator );
    }
    not_in_box.clear();

    typename Teuchos::Array<GlobalOrdinal>::iterator targets_bound =
	std::remove( targets_in_box.begin(), targets_in_box.end(), -1 );
    GlobalOrdinal targets_in_box_size = 
	std::distance( targets_in_box.begin(), targets_bound );

    targets_in_box.resize( targets_in_box_size );
    rendezvous_procs.resize( targets_in_box_size );

    // Via an inverse communication operation, move the global point ordinals
    // that are in the rendezvous decomposition box to the rendezvous
    // decomposition.
    Teuchos::ArrayView<const GlobalOrdinal> targets_in_box_view = 
	targets_in_box();
    Tpetra::Distributor target_to_rendezvous_distributor( d_comm );
    GlobalOrdinal num_rendezvous_points = 
	target_to_rendezvous_distributor.createFromSends( rendezvous_procs() );
    Teuchos::Array<GlobalOrdinal> rendezvous_points( num_rendezvous_points );
    target_to_rendezvous_distributor.doPostsAndWaits( 
	targets_in_box_view, 1, rendezvous_points() );

    // Setup target-to-rendezvous communication.
    Teuchos::ArrayView<const GlobalOrdinal> rendezvous_points_view =
	rendezvous_points();
    RCP_TpetraMap rendezvous_coords_map = 
	Tpetra::createNonContigMap<GlobalOrdinal>(
	    rendezvous_points_view, d_comm );
    Tpetra::Export<GlobalOrdinal> 
	target_to_rendezvous_exporter( d_target_map, rendezvous_coords_map );

    // Move the target coordinates to the rendezvous decomposition.
    GlobalOrdinal num_points = target_ordinals.size();
    Teuchos::RCP< Tpetra::MultiVector<double,GlobalOrdinal> > 
	target_coords =	Tpetra::createMultiVectorFromView( 
	    d_target_map, coords_view, num_points, coord_dim );
    Tpetra::MultiVector<double,GlobalOrdinal> rendezvous_coords( 
	rendezvous_coords_map, coord_dim );
    rendezvous_coords.doExport( *target_coords, target_to_rendezvous_exporter, 
				Tpetra::INSERT );

    // Search the rendezvous decomposition with the target points to get the
    // source elements that contain them.
    Teuchos::Array<GlobalOrdinal> rendezvous_elements;
    Teuchos::Array<int> rendezvous_element_src_procs;
    rendezvous.elementsContainingPoints( rendezvous_coords.get1dViewNonConst(),
					 rendezvous_elements,
					 rendezvous_element_src_procs );

    // Get the points that were not in the mesh. If we're keeping track of
    // missed points, also make a list of those ordinals.
    GlobalOrdinal target_index;
    Teuchos::Array<GlobalOrdinal> not_in_mesh;
    Teuchos::Array<GlobalOrdinal> missed_in_mesh_idx;
    Teuchos::Array<GlobalOrdinal> missed_in_mesh_ordinal;
    typename Teuchos::Array<GlobalOrdinal>::const_iterator 
	rendezvous_elements_iterator;
    typename Teuchos::Array<GlobalOrdinal>::const_iterator 
	rendezvous_elements_begin = rendezvous_elements.begin();
    for ( rendezvous_elements_iterator = rendezvous_elements.begin();
	  rendezvous_elements_iterator != rendezvous_elements.end();
	  ++rendezvous_elements_iterator )
    {
	if ( *rendezvous_elements_iterator == -1 )
	{
	    target_index = std::distance( rendezvous_elements_begin, 
					  rendezvous_elements_iterator );

	    not_in_mesh.push_back( target_index );

	    if ( d_store_missed_points )
	    {
		missed_in_mesh_idx.push_back( target_index );
		missed_in_mesh_ordinal.push_back( 
		    rendezvous_points[target_index] );
	    }
	}
    }

    // If we're keeping track of missed points, send their global ordinals
    // back to the target decomposition so that we can add them to the list.
    if ( d_store_missed_points )
    {
	// Extract the missed point target procs from the target-to-rendezvous
	// distributor.
	Teuchos::ArrayView<const int> from_images = 
	    target_to_rendezvous_distributor.getImagesFrom();
	Teuchos::ArrayView<const std::size_t> from_lengths = 
	    target_to_rendezvous_distributor.getLengthsFrom();
	Teuchos::Array<int> point_target_procs;
	for ( int i = 0; i < (int) from_images.size(); ++i )
	{
	    for ( std::size_t j = 0; j < from_lengths[i]; ++j )
	    {
		point_target_procs.push_back( from_images[i] );
	    }
	}
	testInvariant( point_target_procs.size() == num_rendezvous_points );

	// Build a list of target procs for the missed points.
	Teuchos::Array<int> missed_target_procs( missed_in_mesh_idx.size() );
	for ( int n = 0; n < (int) missed_in_mesh_idx.size(); ++n )
	{
	    missed_target_procs[n] = 
		point_target_procs[ missed_in_mesh_idx[n] ];
	}
	point_target_procs.clear();

	// Send the missed points back to the target decomposition through an
	// inverse communication operation and add them to the list.
	Teuchos::ArrayView<const GlobalOrdinal> missed_in_mesh_ordinal_view = 
	    missed_in_mesh_ordinal();
	Tpetra::Distributor target_to_rendezvous_distributor( d_comm );
	GlobalOrdinal num_missed_targets = 
	    target_to_rendezvous_distributor.createFromSends( 
		missed_target_procs() );
	int offset = d_missed_points.size();
	d_missed_points.resize( offset + num_missed_targets );
	target_to_rendezvous_distributor.doPostsAndWaits( 
	    missed_in_mesh_ordinal_view, 1, 
	    d_missed_points.view( offset, num_missed_targets ) );

	// Convert the missed point global indices to local indices and add
	// them to the list.
	for ( int n = offset; n < offset+num_missed_targets; ++n )
	{
	    d_missed_points[n] = 
		d_target_g2l.find( d_missed_points[n] )->second;
	}
    }
    missed_in_mesh_idx.clear();
    missed_in_mesh_ordinal.clear();

    // Extract the points we didn't find in any elements in the rendezvous
    // decomposition and their corresponding elements. We don't want to send
    // these to the source.
    std::reverse( not_in_mesh.begin(), not_in_mesh.end() );
    typename Teuchos::Array<GlobalOrdinal>::const_iterator not_in_mesh_iterator;
    for ( not_in_mesh_iterator = not_in_mesh.begin();
	  not_in_mesh_iterator != not_in_mesh.end();
	  ++not_in_mesh_iterator )
    {
	rendezvous_points.remove( *not_in_mesh_iterator );
    }
    not_in_mesh.clear();

    typename Teuchos::Array<GlobalOrdinal>::iterator rendezvous_elements_bound =
	std::remove( rendezvous_elements.begin(), 
		     rendezvous_elements.end(), -1 );
    GlobalOrdinal rendezvous_elements_size = 
	std::distance( rendezvous_elements.begin(), rendezvous_elements_bound );

    typename Teuchos::Array<int>::iterator rendezvous_element_src_procs_bound =
	std::remove( rendezvous_element_src_procs.begin(), 
		     rendezvous_element_src_procs.end(), -1 );
    GlobalOrdinal rendezvous_element_src_procs_size = 
	std::distance( rendezvous_element_src_procs.begin(), 
		       rendezvous_element_src_procs_bound );
    testInvariant( rendezvous_elements_size == 
		   rendezvous_element_src_procs_size );

    rendezvous_elements.resize( rendezvous_elements_size );
    rendezvous_element_src_procs.resize( rendezvous_element_src_procs_size );
    rendezvous_points.resize( rendezvous_elements_size );

    // Setup rendezvous-to-source distributor.
    Tpetra::Distributor rendezvous_to_src_distributor( d_comm );
    GlobalOrdinal num_source_elements = 
	rendezvous_to_src_distributor.createFromSends( 
	    rendezvous_element_src_procs() );

    // Send the rendezvous elements to the source decomposition via inverse
    // communication. 
    Teuchos::ArrayView<const GlobalOrdinal> rendezvous_elements_view =
	rendezvous_elements();
    d_source_elements.resize( num_source_elements );
    rendezvous_to_src_distributor.doPostsAndWaits( rendezvous_elements_view, 1,
						   d_source_elements() );

    // Send the rendezvous point global ordinals to the source decomposition
    // via inverse communication.
    Teuchos::ArrayView<const GlobalOrdinal> reduced_rendezvous_points_view =
	rendezvous_points();
    Teuchos::Array<GlobalOrdinal> source_points( num_source_elements );
    rendezvous_to_src_distributor.doPostsAndWaits( 
	reduced_rendezvous_points_view, 1, source_points() );

    // Build the source map from the target ordinals.
    Teuchos::ArrayView<const GlobalOrdinal> source_points_view = 
	source_points();
    d_source_map = Tpetra::createNonContigMap<GlobalOrdinal>( 
	source_points_view, d_comm );
    testPostcondition( d_source_map != Teuchos::null );

    // Send the rendezvous point coordinates to the source decomposition.
    Tpetra::Export<GlobalOrdinal> rendezvous_to_source_exporter( 
	rendezvous_coords_map, d_source_map );
    d_target_coords.resize( num_source_elements*coord_dim );
    Teuchos::RCP< Tpetra::MultiVector<double,GlobalOrdinal> >
	source_coords = Tpetra::createMultiVectorFromView( 
	    d_source_map, Teuchos::arcpFromArray( d_target_coords ), 
	    num_source_elements, coord_dim );
    source_coords->doExport( rendezvous_coords, rendezvous_to_source_exporter,
			     Tpetra::INSERT );

    // Build the source-to-target exporter.
    d_source_to_target_exporter = 
	Teuchos::rcp( new Tpetra::Export<GlobalOrdinal>(
			  d_source_map, d_target_map ) );
    testPostcondition( d_source_to_target_exporter != Teuchos::null );
}

//---------------------------------------------------------------------------//
/*!
 * \brief Get the points missed in the map generation.
 *
 * \return If store_missed_points is true, return the local indices of the
 *  points provided by target_coord_manager that were not mapped. An exception
 *  will be thrown if store_missed_points is false. Returns a null view if all
 *  points have been mapped or the map has not yet been generated.
*/
template<class Mesh, class CoordinateField>
Teuchos::ArrayView<const typename 
		   SharedDomainMap<Mesh,CoordinateField>::GlobalOrdinal> 
SharedDomainMap<Mesh,CoordinateField>::getMissedTargetPoints() const
{
    testPrecondition( d_store_missed_points );
    
    return d_missed_points();
}

//---------------------------------------------------------------------------//
/*!
 * \brief Get the points missed in the map generation.
 *
 * \return If store_missed_points is true, return the local indices of the
 *  points provided by target_coord_manager that were not mapped. An exception
 *  will be thrown if store_missed_points is false. Returns a null view if all
 *  points have been mapped or the map has not yet been generated.
*/
template<class Mesh, class CoordinateField>
Teuchos::ArrayView<typename 
		   SharedDomainMap<Mesh,CoordinateField>::GlobalOrdinal> 
SharedDomainMap<Mesh,CoordinateField>::getMissedTargetPoints()
{
    testPrecondition( d_store_missed_points );
    
    return d_missed_points();
}

//---------------------------------------------------------------------------//
/*!
 * \brief Apply the shared domain map for a valid source field evaluator and
 * target data space to the target points that were mapped.
 *
 * \param source_evaluator Function evaluator used to apply the mapping. This
 * FieldEvaluator must be valid for the source mesh used to generate the map.
 *
 * \param target_space_manager Target space into which the function
 * evaluations will be written. Enough space must be allocated to hold
 * evaluations at all points in all dimensions of the field.
 */
template<class Mesh, class CoordinateField>
template<class SourceField, class TargetField>
void SharedDomainMap<Mesh,CoordinateField>::apply( 
    const Teuchos::RCP< FieldEvaluator<Mesh,SourceField> >& source_evaluator,
    Teuchos::RCP< FieldManager<TargetField> >& target_space_manager )
{
    typedef FieldTraits<SourceField> SFT;
    typedef FieldTraits<TargetField> TFT;

    // Set existence values for the source and target.
    bool source_exists = true;
    if ( source_evaluator.is_null() ) source_exists = false;
    bool target_exists = true;
    if ( target_space_manager.is_null() ) target_exists = false;
    d_comm->barrier();

    // Evaluate the source function at the target points and construct a view
    // of the function evaluations.
    Teuchos::ArrayRCP<typename SFT::value_type> source_field_copy(0,0);
    if ( source_exists )
    {
	SourceField function_evaluations = 
	    source_evaluator->evaluate( 
		Teuchos::arcpFromArray( d_source_elements ),
		Teuchos::arcpFromArray( d_target_coords ) );
	testPrecondition( SFT::dim( function_evaluations ) == d_dimension );

	source_field_copy =    
	    FieldTools<SourceField>::copy( function_evaluations );
    }
    d_comm->barrier();

    // Build a multivector for the function evaluations.
    GlobalOrdinal source_size = source_field_copy.size() / d_dimension;
    Teuchos::RCP<Tpetra::MultiVector<typename SFT::value_type, GlobalOrdinal> > 
	source_vector = Tpetra::createMultiVectorFromView( 
	    d_source_map, source_field_copy, source_size, d_dimension );

    // Construct a view of the target space.
    Teuchos::ArrayRCP<typename TFT::value_type> target_field_view(0,0);
    if ( target_exists )
    {
	target_field_view = FieldTools<TargetField>::nonConstView( 
	    *target_space_manager->field() );
    }
    d_comm->barrier();
    
    // Verify that the target space has the proper amount of memory allocated.
    GlobalOrdinal target_size = target_field_view.size() / d_dimension;
    if ( target_exists )
    {
	testPrecondition( 
	    target_size == 
	    (typename TFT::size_type) d_target_map->getNodeNumElements() );
	testPrecondition( TFT::dim( *target_space_manager->field() ) 
			  == d_dimension );
    }
    d_comm->barrier();

    // Build a multivector for the target space.
    Teuchos::RCP<Tpetra::MultiVector<typename TFT::value_type, GlobalOrdinal> > 
	target_vector =	Tpetra::createMultiVectorFromView( 
	    d_target_map, target_field_view, target_size, d_dimension );

    // Fill the target space with zeros so that points we didn't map get some
    // data.
    if ( target_exists )
    {
	FieldTools<TargetField>::putScalar( 
	    *target_space_manager->field(), 0.0 );
    }
    d_comm->barrier();

    // Move the data from the source decomposition to the target
    // decomposition.
    target_vector->doExport( *source_vector, *d_source_to_target_exporter, 
			     Tpetra::INSERT );
}

//---------------------------------------------------------------------------//
/*!
 * \brief Compute globally unique ordinals for the target points.
 *
 * \param target_coords The coordinates to compute global ordinals for.
 *
 * \param target_ordinals The computed globally unique ordinals for the target
 * coordinates. 
 */
template<class Mesh, class CoordinateField>
void SharedDomainMap<Mesh,CoordinateField>::computePointOrdinals(
    const RCP_CoordFieldManager& target_coord_manager,
    Teuchos::Array<GlobalOrdinal>& target_ordinals )
{
    // Set an existence value for the target coords.
    bool target_exists = true;
    if ( target_coord_manager.is_null() ) target_exists = false;
    int comm_rank = d_comm->getRank();
    GlobalOrdinal local_size = 0;

    if ( target_exists )
    {
	int point_dim = CFT::dim( *target_coord_manager->field() );
	local_size = std::distance( 
	    CFT::begin( *target_coord_manager->field() ),
	    CFT::end( *target_coord_manager->field() ) ) / point_dim;
    }
    d_comm->barrier();

    GlobalOrdinal global_size;
    Teuchos::reduceAll<int,GlobalOrdinal>( *d_comm,
					   Teuchos::REDUCE_MAX,
					   1,
					   &local_size,
					   &global_size );

    target_ordinals.resize( local_size );
    for ( GlobalOrdinal n = 0; n < local_size; ++n )
    {
	target_ordinals[n] = comm_rank*global_size + n;

	// If we're keeping track of missed points, we also need to build the
	// global-to-local ordinal map.
	if ( d_store_missed_points )
	{
	    d_target_g2l[ target_ordinals[n] ] = n;
	}
    }
}

//---------------------------------------------------------------------------//
/*!
 * \brief Get the target points that are in the rendezvous decomposition box.
 *
 * \param target_coords The target coordinates to search the box with.
 *
 * \param target_ordinals The globally unique ordinals for the target
 * coordinates. 
 *
 * \param targets_in_box The global ordinals of the target coordinates in the
 * box. If a target point was not found in the box, return an invalid ordinal,
 * -1, in its positition.
 */
template<class Mesh, class CoordinateField>
void SharedDomainMap<Mesh,CoordinateField>::getTargetPointsInBox(
    const BoundingBox& box, const CoordinateField& target_coords,
    const Teuchos::Array<GlobalOrdinal>& target_ordinals,
    Teuchos::Array<GlobalOrdinal>& targets_in_box )
{
    Teuchos::ArrayRCP<const double> target_coords_view =
	FieldTools<CoordinateField>::view( target_coords );
    GlobalOrdinal dim_size = 
	FieldTools<CoordinateField>::dimSize( target_coords );

    testPrecondition( dim_size == target_ordinals.size() );

    targets_in_box.resize( dim_size );
    int field_dim = CFT::dim( target_coords );
    Teuchos::Array<double> target_point( field_dim );
    for ( GlobalOrdinal n = 0; n < dim_size; ++n )
    {
	for ( int d = 0; d < field_dim; ++d )
	{
	    target_point[d] = target_coords_view[ dim_size*d + n ];
	}

	if ( box.pointInBox( target_point ) )
	{
	    targets_in_box[n] = target_ordinals[n];
	}
	else
	{
	    targets_in_box[n] = -1;
	}

	// If we're keeping track of the points not being mapped, add this
	// point's local index to the list if its not in the box.
	if ( d_store_missed_points && targets_in_box[n] == -1 )
	{
	    d_missed_points.push_back(n);
	}
    }
}

//---------------------------------------------------------------------------//

} // end namespace DataTransferKit

#endif // end DTK_SHAREDDOMAINMAP_DEF_HPP

//---------------------------------------------------------------------------//
// end DTK_SharedDomainMap_def.hpp
//---------------------------------------------------------------------------//