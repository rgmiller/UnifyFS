#include <mpi.h>

#include "unifyfs_configurator.h"
#include "unifyfs_metadata.h"
#include "unifyfs_log.h"
#include "unifyfs_runstate.h"

#include "t/lib/tap.h"

#include "metadata_suite.h"

int main(int argc, char* argv[])
{
    /* need to initialize enougth of the server to use the metadata API */
    unifyfs_cfg_t server_cfg;
    int rc, provided, glb_rank, glb_size;

    /* get the configuration */
    rc = unifyfs_config_init(&server_cfg, argc, argv);
    if (rc != 0) {
        exit(1);
    }

    rc = unifyfs_write_runstate(&server_cfg);
    if (rc != (int)UNIFYFS_SUCCESS) {
        exit(1);
    }

    rc = MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);

    if (rc != MPI_SUCCESS) {
        exit(1);
    }

    rc = MPI_Comm_rank(MPI_COMM_WORLD, &glb_rank);
    if (rc != MPI_SUCCESS) {
        exit(1);
    }

    rc = MPI_Comm_size(MPI_COMM_WORLD, &glb_size);
    if (rc != MPI_SUCCESS) {
        exit(1);
    }

    rc = meta_init_store(&server_cfg);
    if (rc != 0) {
        LOG(LOG_ERR, "%s",
            unifyfs_error_enum_description(UNIFYFS_ERROR_MDINIT));
        exit(1);
    }

    /*
     * necessary infrastructure is initialized
     * running tests
     */

    plan(NO_PLAN);

    // keep the order

    unifyfs_set_file_attribute_test();
    unifyfs_get_file_attribute_test();


    /*
     * shut down infrastructure
     */

    // shutdown the metadata service
    meta_sanitize();

    // finalize mpi
    MPI_Finalize();

    // finish the testing
    // needs to be last call
    done_testing();

    return EXIT_SUCCESS;
}
