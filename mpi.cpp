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

/***************************************************************************
 * Authors:     J.M. de la Rosa Trevin (jmdelarosa@cnb.csic.es)
 *
 *
 * Unidad de  Bioinformatica of Centro Nacional de Biotecnologia , CSIC
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307  USA
 *
 *  All comments concerning this program package may be sent to the
 *  e-mail address 'xmipp@cnb.csic.es'
 ***************************************************************************/

#include "mpi.h"

//------------ MPI ---------------------------
MpiNode::MpiNode(int &argc, char ** argv)
{
    //MPI Initialization
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    // Handle errors
    MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN);

    // Set up Slave communicator -----------------------------------------
    MPI_Comm_group(MPI_COMM_WORLD, &worldG);
    int mstr[1]  = {0};
    MPI_Group_excl( worldG, 1, mstr, &slaveG ); // exclude master
    MPI_Comm_create(MPI_COMM_WORLD, slaveG, &slaveC);
    if(rank!=0)
    	MPI_Group_rank(slaveG, &slaveRank);
    else
    	slaveRank = -1;
    // -------------------------------------------------------------------
    color=3;
    if(rank!=0)
    {
    	rank %2 !=0 ? color=1 : color=2;
    }
    MPI_Comm_split(MPI_COMM_WORLD, color, rank, &group_comm);
}

MpiNode::~MpiNode()
{
    MPI_Finalize();
}

bool MpiNode::isMaster() const
{
    return rank == 0;
}

int MpiNode::myRandomSubset() const
{
	if (rank == 0)
		return 0;
	else
		return (rank % 2 == 0) ? 2 : 1;
}

std::string MpiNode::getHostName() const
{
    char nodename[64] = "undefined";
    gethostname(nodename,sizeof(nodename));
    std::string result(nodename);
    return result;

}

void MpiNode::barrierWait()
{
  MPI_Barrier(MPI_COMM_WORLD);
}

// MPI_TEST will be executed every this many seconds: so this determines the minimum time taken for every send operation!!
//#define VERBOSE_MPISENDRECV
int MpiNode::relion_MPI_Send(void *buf, std::ptrdiff_t count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm) {

    int result(0);
    RFLOAT start_time = MPI_Wtime();

//#define ONLY_NORMAL_SEND
//#ifdef ONLY_NORMAL_SEND
    int unitsize(0);
    MPI_Type_size(datatype, &unitsize);
    const std::ptrdiff_t blocksize(512*1024*1024);
    const std::ptrdiff_t totalsize(count*unitsize);
    if (totalsize <= blocksize ) {
        result = MPI_Send(buf, count, datatype, dest, tag, comm);
        if (result != MPI_SUCCESS) {
            report_MPI_ERROR(result);
        }
    } else {
        char * const buffer(reinterpret_cast<char*>(buf));
        const std::ptrdiff_t ntimes(totalsize/blocksize);
        const std::ptrdiff_t nremain(totalsize%blocksize);
//        if(rank == 1)
//        printf("send divide block %d \n",ntimes);
        std::ptrdiff_t i(0);
        for(; i<ntimes; ++i) {
            result = MPI_Send(buffer+i*blocksize, blocksize, MPI_CHAR, dest, tag, comm);
            if (result != MPI_SUCCESS) {
                report_MPI_ERROR(result);
            }
        }
        if(nremain>0) {
            result = MPI_Send(buffer+i*blocksize, nremain, MPI_CHAR, dest, tag, comm);
            if (result != MPI_SUCCESS) {
                report_MPI_ERROR(result);
            }
        }
    }
/*
#else
        // Only use Bsend for larger messages, otherwise use normal send
        if (count > 100) {
                int size;
                MPI_Pack_size( count, datatype, comm, &size );
                char *membuff;

                // Allocate memory for the package to be sent
                int attach_result = MPI_Buffer_attach( malloc(size + MPI_BSEND_OVERHEAD ), size + MPI_BSEND_OVERHEAD );
                if (attach_result != MPI_SUCCESS)
                {
                        report_MPI_ERROR(result);
                }

                // Actually start sending the message
                result = MPI_Bsend(buf, count, datatype, dest, tag, comm);
                if (result != MPI_SUCCESS)
                {
                        report_MPI_ERROR(result);
                }

                // The following will only complete once the message has been successfully sent (i.e. also received on the other side)
                int deattach_result = MPI_Buffer_detach( &membuff, &size);
                if (deattach_result != MPI_SUCCESS)
                {
                        report_MPI_ERROR(result);
                }
        } else {
                result = MPI_Send(buf, count, datatype, dest, tag, comm);
                if (result != MPI_SUCCESS)
                {
                        report_MPI_ERROR(result);
                }
        }
#endif
*/

#ifdef VERBOSE_MPISENDRECV
        if (count > 100)
                std::cerr <<" relion_MPI_Send: message to " << dest << " of size "<< count << " arrived in " << MPI_Wtime() - start_time << " seconds" << std::endl;
#endif
        return result;

}

int MpiNode::relion_MPI_Recv(void *buf, std::ptrdiff_t count, MPI_Datatype datatype, int source, int tag, MPI_Comm comm, MPI_Status &status) {
    int result;
    MPI_Request request;
    RFLOAT current_time = MPI_Wtime();
    RFLOAT start_time = current_time;

    int unitsize(0);
    MPI_Type_size(datatype, &unitsize);
    const std::ptrdiff_t blocksize(512*1024*1024);
    const std::ptrdiff_t totalsize(count*unitsize);
    if (totalsize <= blocksize ) {
        int result_irecv = MPI_Irecv(buf, count, datatype, source, tag, comm, &request);
        if (result_irecv != MPI_SUCCESS) {
            report_MPI_ERROR(result_irecv);
        }

        result = MPI_Wait(&request, &status);
        if (result != MPI_SUCCESS) {
            report_MPI_ERROR(result);
        }
    } else {
        char * const buffer(reinterpret_cast<char*>(buf));
        const std::ptrdiff_t ntimes(totalsize/blocksize);
        const std::ptrdiff_t nremain(totalsize%blocksize);
        std::ptrdiff_t i(0);
        for(; i<ntimes; ++i) {
            int result_irecv = MPI_Irecv(buffer+i*blocksize, blocksize, MPI_CHAR, source, tag, comm, &request);
            if (result_irecv != MPI_SUCCESS) {
                report_MPI_ERROR(result_irecv);
            }

            result = MPI_Wait(&request, &status);
            if (result != MPI_SUCCESS) {
                report_MPI_ERROR(result);
            }
        }
        if(nremain>0) {
            int result_irecv = MPI_Irecv(buffer+i*blocksize, nremain, MPI_CHAR, source, tag, comm, &request);
            if (result_irecv != MPI_SUCCESS) {
                report_MPI_ERROR(result_irecv);
            }

            result = MPI_Wait(&request, &status);
            if (result != MPI_SUCCESS) {
                report_MPI_ERROR(result);
            }
        }
    }
/*
        // First make a non-blocking receive
        int result_irecv = MPI_Irecv(buf, count, datatype, source, tag, comm, &request);
        if (result_irecv != MPI_SUCCESS)
        {
                report_MPI_ERROR(result_irecv);
        }

        // I could do something in between. If not, Irecv == Recv
        // Wait for it to finish (MPI_Irecv + MPI_Wait == MPI_Recv)
        result = MPI_Wait(&request, &status);
        if (result != MPI_SUCCESS)
        {
                report_MPI_ERROR(result);
        }
*/
#ifdef VERBOSE_MPISENDRECV
        if (count > 100)
                std::cerr <<" relion_MPI_Recv: message from "<<source << " of size "<< count <<" arrived in " << MPI_Wtime() - start_time << " seconds" << std::endl;
#endif
        return result;

}


int MpiNode::relion_MPI_Bcast(void *buffer, long int count, MPI_Datatype datatype, int root, MPI_Comm comm)
{
	int result;
    int unitsize(0);
    MPI_Type_size(datatype, &unitsize);
    const long blocksize(1 * 1024 * 1024 * 1024);
    const long totalsize(count * unitsize);

    if (count < 0) report_MPI_ERROR(MPI_ERR_COUNT);  // overflow
    if (totalsize <= blocksize) {
        // maximum amount of data can be sent by MPI_Bcast
        // 2 * 1024 * 1024 * 1024 - 1 = 2^31 - 1 = 2147483647 bytes
        result = MPI_Bcast(buffer, static_cast<int>(count), datatype, root, comm);
        if (result != MPI_SUCCESS) {
            report_MPI_ERROR(result);
        }
    } else {
        if (rank == root) {
            for (int dest = 0; dest < size; dest++) {   
                if (dest != root) {
                    result = relion_MPI_Send(buffer, count, datatype, dest, MPITAG_BCAST, comm);
                    if (result != MPI_SUCCESS) {
                        report_MPI_ERROR(result);
                    }
                }
            }
        } else {
            MPI_Status status;
            result = relion_MPI_Recv(buffer, count, datatype, root, MPITAG_BCAST, comm, status);
            if (result != MPI_SUCCESS) {
                report_MPI_ERROR(result);
            }
        }
    }

	return result;

}

void MpiNode::report_MPI_ERROR(int error_code)
{
	char error_string[200];
	int length_of_error_string, error_class;
	MPI_Error_class(error_code, &error_class);
	MPI_Error_string(error_class, error_string, &length_of_error_string);
	fprintf(stderr, "%3d: %s\n", rank, error_string);
	MPI_Error_string(error_code, error_string, &length_of_error_string);
	fprintf(stderr, "%3d: %s\n", rank, error_string);

	std::cerr.flush();
	REPORT_ERROR("Encountered an MPI-related error, see above. Now exiting...");

}

int MpiNode::relion_MPI_Allreduce(void* send_data,void* recv_data,std::ptrdiff_t count,MPI_Datatype datatype,MPI_Op op,MPI_Comm communicator)
{

    int result(0);
    RFLOAT start_time = MPI_Wtime();

//#define ONLY_NORMAL_SEND
//#ifdef ONLY_NORMAL_SEND
    int unitsize(0);
    MPI_Type_size(datatype, &unitsize);
    const std::ptrdiff_t blocksize(512*1024*1024);
    const std::ptrdiff_t totalsize(count*unitsize);
    if (totalsize <= blocksize ) {
        result = MPI_Allreduce(send_data,recv_data,count,datatype,op,communicator);
        if (result != MPI_SUCCESS) {
            report_MPI_ERROR(result);
        }
    } else {

    	const std::ptrdiff_t ntimes(totalsize/blocksize);
    	int offset=blocksize/unitsize; //offset = blocknum;
    	const std::ptrdiff_t nremain(totalsize%blocksize);
    	int remainnum=nremain/unitsize;
        std::ptrdiff_t i(0);
        for(; i<ntimes; ++i) {
            result = MPI_Allreduce(send_data+i*offset,recv_data+i*offset,offset,datatype,op,communicator);
            if (result != MPI_SUCCESS) {
                report_MPI_ERROR(result);
            }
        }
        if(remainnum>0) {
            result = MPI_Allreduce(send_data+i*offset,recv_data+i*offset,remainnum,datatype,op,communicator);
            if (result != MPI_SUCCESS) {
                report_MPI_ERROR(result);
            }
        }

/*        char * const buffer(reinterpret_cast<char*>(buf));
        const std::ptrdiff_t nremain(totalsize%blocksize);
        std::ptrdiff_t i(0);
        for(; i<ntimes; ++i) {
            result = MPI_Allreduce(send_data+i*blocksize,recv_data+i*blocksize,blocksize,MPI_CHAR,op,communicator);
            		MPI_Send(buffer+i*blocksize, blocksize, MPI_CHAR, dest, tag, comm);
            if (result != MPI_SUCCESS) {
                report_MPI_ERROR(result);
            }
        }
        if(nremain>0) {
            result = MPI_Send(buffer+i*blocksize, nremain, MPI_CHAR, dest, tag, comm);
            if (result != MPI_SUCCESS) {
                report_MPI_ERROR(result);
            }
        }*/
    }
/*
#else
        // Only use Bsend for larger messages, otherwise use normal send
        if (count > 100) {
                int size;
                MPI_Pack_size( count, datatype, comm, &size );
                char *membuff;
                // Allocate memory for the package to be sent
                int attach_result = MPI_Buffer_attach( malloc(size + MPI_BSEND_OVERHEAD ), size + MPI_BSEND_OVERHEAD );
                if (attach_result != MPI_SUCCESS)
                {
                        report_MPI_ERROR(result);
                }
                // Actually start sending the message
                result = MPI_Bsend(buf, count, datatype, dest, tag, comm);
                if (result != MPI_SUCCESS)
                {
                        report_MPI_ERROR(result);
                }
                // The following will only complete once the message has been successfully sent (i.e. also received on the other side)
                int deattach_result = MPI_Buffer_detach( &membuff, &size);
                if (deattach_result != MPI_SUCCESS)
                {
                        report_MPI_ERROR(result);
                }
        } else {
                result = MPI_Send(buf, count, datatype, dest, tag, comm);
                if (result != MPI_SUCCESS)
                {
                        report_MPI_ERROR(result);
                }
        }
#endif
*/

#ifdef VERBOSE_MPISENDRECV
        if (count > 100)
                std::cerr <<" relion_MPI_Send: message to " << dest << " of size "<< count << " arrived in " << MPI_Wtime() - start_time << " seconds" << std::endl;
#endif
        return result;

}

int MpiNode::relion_MPI_Allreduce_float(RFLOAT* send_data,RFLOAT* recv_data,long int count,MPI_Datatype datatype,MPI_Op op,MPI_Comm communicator)
{

    int result(0);
    RFLOAT start_time = MPI_Wtime();

//#define ONLY_NORMAL_SEND
//#ifdef ONLY_NORMAL_SEND
    int unitsize(0);
    MPI_Type_size(datatype, &unitsize);
    const std::ptrdiff_t blocksize(512*1024*1024);
    const std::ptrdiff_t totalsize(count*unitsize);
    if (totalsize <= blocksize ) {
        result = MPI_Allreduce(send_data,recv_data,count,datatype,op,communicator);
        if (result != MPI_SUCCESS) {
            report_MPI_ERROR(result);
        }
    } else {


    	const std::ptrdiff_t ntimes(totalsize/blocksize);
    	int offset=blocksize/unitsize; //offset = blocknum;
    	const std::ptrdiff_t nremain(totalsize%blocksize);
    	int remainnum=nremain/unitsize;
    	//printf("ALL reduce offset :%d remainnum :%d ntimes :%d\n",offset,remainnum,ntimes);
        std::ptrdiff_t i(0);
        for(; i<ntimes; ++i) {
            result = MPI_Allreduce(send_data+i*offset,recv_data+i*offset,offset,datatype,op,communicator);
            if (result != MPI_SUCCESS) {
                report_MPI_ERROR(result);
            }
        }
        if(remainnum>0) {
            result = MPI_Allreduce(send_data+i*offset,recv_data+i*offset,remainnum,datatype,op,communicator);
            if (result != MPI_SUCCESS) {
                report_MPI_ERROR(result);
            }
        }

/*        char * const buffer(reinterpret_cast<char*>(buf));
        const std::ptrdiff_t nremain(totalsize%blocksize);
        std::ptrdiff_t i(0);
        for(; i<ntimes; ++i) {
            result = MPI_Allreduce(send_data+i*blocksize,recv_data+i*blocksize,blocksize,MPI_CHAR,op,communicator);
            		MPI_Send(buffer+i*blocksize, blocksize, MPI_CHAR, dest, tag, comm);
            if (result != MPI_SUCCESS) {
                report_MPI_ERROR(result);
            }
        }
        if(nremain>0) {
            result = MPI_Send(buffer+i*blocksize, nremain, MPI_CHAR, dest, tag, comm);
            if (result != MPI_SUCCESS) {
                report_MPI_ERROR(result);
            }
        }*/
    }
/*
#else
        // Only use Bsend for larger messages, otherwise use normal send
        if (count > 100) {
                int size;
                MPI_Pack_size( count, datatype, comm, &size );
                char *membuff;
                // Allocate memory for the package to be sent
                int attach_result = MPI_Buffer_attach( malloc(size + MPI_BSEND_OVERHEAD ), size + MPI_BSEND_OVERHEAD );
                if (attach_result != MPI_SUCCESS)
                {
                        report_MPI_ERROR(result);
                }
                // Actually start sending the message
                result = MPI_Bsend(buf, count, datatype, dest, tag, comm);
                if (result != MPI_SUCCESS)
                {
                        report_MPI_ERROR(result);
                }
                // The following will only complete once the message has been successfully sent (i.e. also received on the other side)
                int deattach_result = MPI_Buffer_detach( &membuff, &size);
                if (deattach_result != MPI_SUCCESS)
                {
                        report_MPI_ERROR(result);
                }
        } else {
                result = MPI_Send(buf, count, datatype, dest, tag, comm);
                if (result != MPI_SUCCESS)
                {
                        report_MPI_ERROR(result);
                }
        }
#endif
*/

#ifdef VERBOSE_MPISENDRECV
        if (count > 100)
                std::cerr <<" relion_MPI_Send: message to " << dest << " of size "<< count << " arrived in " << MPI_Wtime() - start_time << " seconds" << std::endl;
#endif
        return result;

}



void printMpiNodesMachineNames(MpiNode &node, int nthreads)
{


    if (node.isMaster())
    {
    	std::cout << " === RELION MPI setup ===" << std::endl;
    	std::cout << " + Number of MPI processes             = " << node.size << std::endl;
    	if (nthreads > 1)
    	{
    		std::cout << " + Number of threads per MPI process  = " << nthreads << std::endl;
    		std::cout << " + Total number of threads therefore  = " << nthreads * node.size << std::endl;
		}
    	std::cout << " + Master  (0) runs on host            = " << node.getHostName() << std::endl;
    	std::cout.flush();
    }
    node.barrierWait();

    for (int slave = 1; slave < node.size; slave++)
    {
    	if (slave == node.rank)
    	{
    		std::cout << " + Slave ";
    		std::cout.width(5);
    		std::cout << slave;
    		std::cout << " runs on host            = " << node.getHostName() << std::endl;
    		std::cout.flush();
		}
    	node.barrierWait();
    }

    if (node.isMaster())
    {
            std::cout << " =================" << std::endl;
    }
    std::cout.flush();

    // Try to flush all std::cout of all MPI processes before proceeding...
    sleep(1);
    node.barrierWait();

}

