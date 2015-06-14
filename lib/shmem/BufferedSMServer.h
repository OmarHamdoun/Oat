//******************************************************************************
//* Copyright (c) Jon Newman (jpnewman at mit snail edu) 
//* All right reserved.
//* This file is part of the Simple Tracker project.
//* This is free software: you can redistribute it and/or modify
//* it under the terms of the GNU General Public License as published by
//* the Free Software Foundation, either version 3 of the License, or
//* (at your option) any later version.
//* This software is distributed in the hope that it will be useful,
//* but WITHOUT ANY WARRANTY; without even the implied warranty of
//* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//* GNU General Public License for more details.
//* You should have received a copy of the GNU General Public License
//* along with this source code.  If not, see <http://www.gnu.org/licenses/>.
//******************************************************************************

#ifndef BUFFEREDSMSERVER_H
#define	BUFFEREDSMSERVER_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/lockfree/spsc_queue.hpp>

#include "SyncSharedMemoryObject.h"

namespace oat {

    namespace bip = boost::interprocess;

    template<class T, template <typename> class SharedMemType = oat::SyncSharedMemoryObject>
    class BufferedSMServer {
    public:
        BufferedSMServer(std::string sink_name);
        BufferedSMServer(const BufferedSMServer& orig);
        virtual ~BufferedSMServer();

        void createSharedObject(void);
        void pushObject(T value, uint32_t sample_number);


        // Accessors
        bool is_running(void) { return running; }
        void set_running(bool value) { running = value; }

    private:

        // Name of this server
        std::string name;

        // Buffer
        static const int SMSERVER_BUFFER_SIZE = 1024;
        boost::lockfree::spsc_queue
        < std::pair<unsigned int, T>, boost::lockfree::capacity<SMSERVER_BUFFER_SIZE> > buffer;

        // Server threading
        std::thread server_thread;
        std::mutex server_mutex;
        std::condition_variable serve_condition;
        std::atomic<bool> running; // Server running

        // Shared memory and managed object names
        SharedMemType<T>* shared_object; // Defaults to oat::SyncSharedMemoryObject<T>
        std::string shmem_name, shobj_name;
        bip::managed_shared_memory shared_memory;
        bool shared_object_created;

        void createSharedObject(size_t bytes);
        void serveFromBuffer(void);
        void notifySelf(void);
        
#ifndef NDEBUG
        const int BAR_WIDTH = 50;
#endif

    };

    template<class T, template <typename> class SharedMemType>
    BufferedSMServer<T, SharedMemType>::BufferedSMServer(std::string sink_name) :
    name(sink_name)
    , shmem_name(sink_name + "_sh_mem")
    , shobj_name(sink_name + "_sh_obj")
    , shared_object_created(false)
    , running(true) {

        // Start the server thread
        server_thread = std::thread(&BufferedSMServer<T, SharedMemType>::serveFromBuffer, this);
    }

    template<class T, template <typename> class SharedMemType>
    BufferedSMServer<T, SharedMemType>::BufferedSMServer(const BufferedSMServer<T, SharedMemType>& orig) {
    }

    template<class T, template <typename> class SharedMemType>
    BufferedSMServer<T, SharedMemType>::~BufferedSMServer() {

        running = false;

        // Make sure we unblock the server thread
        for (int i = 0; i <= SMSERVER_BUFFER_SIZE; ++i) {
            notifySelf();
        }

        // Join the server thread back with the main one
        server_thread.join();

        // Remove_shared_memory on object destruction
        bip::shared_memory_object::remove(shmem_name.c_str());
#ifndef NDEBUG
        std::cout << "Shared memory \'" + shmem_name + "\' was deallocated.\n";
#endif
    }

    template<class T, template <typename> class SharedMemType>
    void BufferedSMServer<T, SharedMemType>::createSharedObject() {

        try {

            // Allocate shared memory
            shared_memory = bip::managed_shared_memory(
                    bip::open_or_create,
                    shmem_name.c_str(),
                    sizeof (SharedMemType<T>) + 1024);

            // Make the shared object
            shared_object = shared_memory.find_or_construct<SharedMemType < T >> (shobj_name.c_str())();


        } catch (bip::interprocess_exception& ex) {
            std::cerr << ex.what() << '\n';
            exit(EXIT_FAILURE); // TODO: exit does not unwind the stack to take care of destructing shared memory objects
        }

        shared_object_created = true;
    }

    /**
     * Push object into shared memory FIFO buffer.
     * 
     * @param value Object to store in shared memory. This object is copied on the FIFO.
     * @param sample_number The sample number associated with the object copied onto the FIFO.
     */
    template<class T, template <typename> class SharedMemType>
    void BufferedSMServer<T, SharedMemType>::pushObject(T value, uint32_t sample_number) {

        // Push data onto ring buffer
        buffer.push(std::make_pair(sample_number, value));

        // notify server thread that data is available
        serve_condition.notify_one();

    }

    template<class T, template <typename> class SharedMemType>
    void BufferedSMServer<T, SharedMemType>::serveFromBuffer() {

        while (running) {

            // Proceed only if mat_buffer has data
            std::unique_lock<std::mutex> lk(server_mutex);
            serve_condition.wait_for(lk, std::chrono::milliseconds(10));

            std::pair<uint32_t, T> sample;
            while (buffer.pop(sample) && running) {

#ifndef NDEBUG

                std::cout << "[";

                int progress = (BAR_WIDTH * buffer.read_available()) / SMSERVER_BUFFER_SIZE;
                int remaining = BAR_WIDTH - progress;

                for (int i = 0; i < progress; ++i) {
                    std::cout << "=";
                }
                for (int i = 0; i < remaining; ++i) {
                    std::cout << " ";
                }

                std::cout << "] "
                        << std::to_string(buffer.read_available()) + "/" + std::to_string(SMSERVER_BUFFER_SIZE)
                        << ", sample: " + std::to_string(sample.first)
                        << "\r";

                std::cout.flush();

#endif

                if (!shared_object_created) {
                    createSharedObject();
                }

                /* START CRITICAL SECTION */
                shared_object->mutex.wait();

                // Perform writes in shared memory 
                shared_object->writeSample(sample.first, sample.second);

                shared_object->mutex.post();
                /* END CRITICAL SECTION */

                // Tell each client they can proceed
                for (int i = 0; i < shared_object->number_of_clients; ++i) {
                    shared_object->read_barrier.post();
                }

                // Only wait if there is a client
                if (shared_object->number_of_clients) {
                    shared_object->write_barrier.wait();
                }

                // Tell each client they can proceed now that the write_barrier
                // has been passed
                for (int i = 0; i < shared_object->number_of_clients; ++i) {
                    shared_object->new_data_barrier.post();
                }
            }
        }
    }

    template<class T, template <typename> class SharedMemType>
    void BufferedSMServer<T, SharedMemType>::notifySelf() {

        if (shared_object_created) {
            shared_object->write_barrier.post();
        }
    }
} 

#endif	/* BUFFEREDSMSERVER_H */