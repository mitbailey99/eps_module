/**
 * @file main.c
 * @author Sunip K. Mukherjee (sunipkmukherjee@gmail.com)
 * @brief main() symbol of the SPACE-HAUC Flight Software.
 * @version 0.2
 * @date 2020-03-19
 * 
 * @copyright Copyright (c) 2020
 * 
 */
#define MAIN_PRIVATE       // enable prototypes in main.h and modules in modules.h
#define DATALOGGER_PRIVATE //
#include <main.h>
#include <modules.h>
#undef MAIN_PRIVATE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

int sys_boot_count = -1;
volatile sig_atomic_t done = 0;
__thread int sys_status;

int dlgr_idx = 0;
datalogger_t *dlgr_settings = NULL;
char **dlgr_modname = NULL;

/**
 * @brief Main function executed when shflight.out binary is executed
 * 
 * @return int returns 0 on success, -1 on failure, error code on thread init failures
 */
int main(void)
{
    // Boot counter
    sys_boot_count = bootCount(); // Holds bootCount to generate a different log file at every boot
    if (sys_boot_count < 0)
    {
        fprintf(stderr, "Boot count returned negative, fatal error. Exiting.\n");
        exit(-1);
    }
    // SIGINT handler register
    struct sigaction saction;
    saction.sa_handler = &catch_sigint;
    sigaction(SIGINT, &saction, NULL);

    // Allocate dlgr_settings enough memory to store settings of all systems, even though
    // this is unlikely to ever be needed.
    dlgr_settings = (datalogger_t *)malloc(sizeof(datalogger_t) * num_systems);
    dlgr_modname = (char *)malloc(sizeof(char*) * num_systems * 20);
    
    // initialize modules
    for (int i = 0; i < num_init; i++)
    {
        int val = module_init[i]();
        if (val < 0)
        {
            sherror("Error in initialization!");
            exit(-1);
        }
    }
    printf("Done init modules\n");

    // Initialize datalogger.
    //printf("Initializing datalogger: %d\n", dlgr_init());

    // set up threads
    int rc[num_systems];                                         // fork-join return codes
    pthread_t thread[num_systems];                               // thread containers
    pthread_attr_t attr;                                         // thread attribute
    int args[num_systems];                                       // thread arguments (thread id in this case, but can be expanded by passing structs etc)
    void *status;                                                // thread return value
    pthread_attr_init(&attr);                                    // initialize attribute
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE); // create threads to be joinable

    for (int i = 0; i < num_systems; i++)
    {
        args[i] = i; // sending a pointer to i to every thread may end up with duplicate thread ids because of access times
        rc[i] = pthread_create(&thread[i], &attr, module_exec[i], (void *)(&args[i]));
        if (rc[i])
        {
            printf("[Main] Error: Unable to create thread %d: Errno %d\n", i, errno);
            exit(-1);
        }
    }

    pthread_attr_destroy(&attr); // destroy the attribute

    for (int i = 0; i < num_systems; i++)
    {
        rc[i] = pthread_join(thread[i], &status);
        if (rc[i])
        {
            printf("[Main] Error: Unable to join thread %d: Errno %d\n", i, errno);
            exit(-1);
        }
    }

    // destroy modules
    for (int i = 0; i < num_destroy; i++)
    {
        module_destroy[i]();
    }

    free(dlgr_settings);

    return 0;
}
/**
 * @brief SIGINT handler, sets the global variable `done` as 1, so that thread loops can break.
 * Wakes up sitl_comm and datavis threads to ensure they exit.
 * 
 * @param sig Receives the signal as input.
 */
void catch_sigint(int sig)
{
    done = 1;
    for (int i = 0; i < num_wakeups; i++)
        pthread_cond_broadcast(wakeups[i]);
}
/**
 * @brief Prints errors specific to shflight in a fashion similar to perror
 * 
 * @param msg Input message to print along with error description
 */
void sherror(const char *msg)
{
    switch (sys_status)
    {
    case ERROR_MALLOC:
        fprintf(stderr, "%s: Error allocating memory\n", msg);
        break;

    case ERROR_HBRIDGE_INIT:
        fprintf(stderr, "%s: Error initializing h-bridge\n", msg);
        break;

    case ERROR_MUX_INIT:
        fprintf(stderr, "%s: Error initializing mux\n", msg);
        break;

    case ERROR_CSS_INIT:
        fprintf(stderr, "%s: Error initializing CSS\n", msg);
        break;

    case ERROR_FSS_INIT:
        fprintf(stderr, "%s: Error initializing FSS\n", msg);
        break;

    case ERROR_FSS_CONFIG:
        fprintf(stderr, "%s: Error configuring FSS\n", msg);
        break;

    default:
        fprintf(stderr, "%s\n", msg);
        break;
    }
}

int bootCount()
{
    FILE *fp;
    int _bootCount = 0;                      // assume 0 boot
    if (access(BOOTCOUNT_FNAME, F_OK) != -1) // file exists
    {
        fp = fopen(BOOTCOUNT_FNAME, "r+");              // open file for reading
        int read_bytes = fscanf(fp, "%d", &_bootCount); // read bootcount
        if (read_bytes < 0)                             // if no bytes were read
        {
            perror("File not read"); // indicate error
            _bootCount = 0;          // reset boot count
        }
        fclose(fp); // close
    }
    // Update boot file
    fp = fopen(BOOTCOUNT_FNAME, "w"); // open for writing
    fprintf(fp, "%d", ++_bootCount);  // write 1
    fclose(fp);                       // close
    sync();                           // sync file system
    return --_bootCount;              // return 0 on first boot, return 1 on second boot etc
}

/***************************
 * 
 * DATALOGGER
 * 
 * *************************/

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define FBEGIN_SIZE 6
#define FEND_SIZE 4

char FBEGIN[6] = {'F', 'B', 'E', 'G', 'I', 'N'};
char FEND[4] = {'F', 'E', 'N', 'D'};

// Example Directory
/* datalogger
 * - log
 * - - eps
 * - - - settings.cfg
 * - - - index.inf
 * - - - module.inf
 * - - - 0.dat
 * - - - 1.dat
 * - - - 2.dat
 * - - - 3.dat
 * - - - 4.dat
 * - - - 5.dat
 * - - acs
 * - - - settings.cfg
 * - - - index.inf
 * - - - module.inf
 * - - - 0.dat
 * - - - 1.dat
 */

// settings.cfg file format (index moved to index.inf)
/* 1. max file size (Bytes)
 * 2. max dir size (Bytes)
 */

// char* moduleName is just a placeholder. Later, we will get the
// module names from somewhere else.

int dlgr_init(char* moduleName, ssize_t maxLogSize)
{
    eprintf("DEBUG: dlgr_init called...");

    if (maxLogSize < 1){
        return ERR_INVALID_INPUT;
    }

    // Store the moduleName into dlgr_modname[dlgr_idx];
    dlgr_modname[dlgr_idx] = moduleName;

    // Check if log directory exists. If not, create it.
    const char directory[] = "log";
    struct stat sb;

    if (!(stat(directory, &sb) == 0 && S_ISDIR(sb.st_mode)))
    {
        mkdir(directory, S_IRUSR | S_IWUSR);
    }

    // Change our directory to inside the log folder.
    if (chdir(directory) < 0){
        return ERR_CHDIR_FAIL;
    }

    eprintf("DEBUG: Passed log directory check.");

    // Check if moduleName directory exists. If not, create it.
    if (!(stat(moduleName, &sb) == 0 && S_ISDIR(sb.st_mode))){
        mkdir(moduleName, S_IRUSR | S_IWUSR);
    }

    // Change our directory to inside the moduleName folder.
    if (chdir(moduleName) < 0){
        return ERR_CHDIR_FAIL;
    }

    eprintf("DEBUG: Passed moduleName directory check.");

#define MODULE_FNAME_SZ 128

    // Set up module.inf file.
    FILE *modu = NULL;
    const char* moduleFile = "module.inf";

    // To retrieve the maxLogSize as a c-string.
    char sMaxLogSize[20];

    // If a module.inf file already exists, use its maxLogSize value. 
    // Otherwise, create one and put in this module's max log size.
    if (access(moduleFile, F_OK | R_OK) == 0){
        modu = fopen(moduleFile, "r");
        if (modu == NULL){
            return ERR_MODU_OPEN;
        }
        fgets(sMaxLogSize, 20, (FILE *)modu);
        dlgr_settings[dlgr_idx].moduleLogSize = atoi(sMaxLogSize);
    } else {
        modu = fopen(moduleFile, "w");
        if (modu == NULL){
            return ERR_MODU_OPEN;
        }
        fprintf(modu, "%d", maxLogSize);
        dlgr_settings[dlgr_idx].moduleLogSize = maxLogSize;
        fclose(modu);
        sync();
    }

    eprintf("DEBUG: Passed module.inf check.");

    // Set up index.inf file.
    FILE *index = NULL;
    const char* indexFile = "index.inf";

    // To retrieve the index as a c-string.
    char sIndex[20];

    // If there is no index.inf, then there cannot be any .dat log files either.
    // If this is the case, we will also need to create an initial 0.dat file.
    FILE *data = NULL;
    const char* dataFile = "0.dat";

    // If an index.inf exists, grab our last index.
    // Otherwise, put in 0.
    if (access(indexFile, F_OK | R_OK) == 0){
        index = fopen(indexFile, "r");
        if (index == NULL){
            return ERR_INDEX_OPEN;
        }
        fgets(sIndex, 20, (FILE *)index);
        dlgr_settings[dlgr_idx].logIndex = atoi(sIndex);
        fclose(index);
    } else {
        index = fopen(indexFile, "w");
        if (index == NULL){
            return ERR_INDEX_OPEN;
        }
        fprintf(index, "0\n");
        dlgr_settings[dlgr_idx].logIndex = 0;
        fclose(index);
        sync();

        // Now set up 0.dat. Note that it will be blank until a log is logged.
        data = fopen(dataFile, "wb");
        if (data == NULL){
            return ERR_DATA_OPEN;
        }
        fclose(data);
        sync();
    }

    eprintf("DEBUG: Passed index.inf and 0.dat check.");

    // Set up settings.cfg file.
    FILE *settings = NULL;
    const char* settingsFile = "settings.cfg";

    // For the extraction of relevant settings.
    char sMaxFileSize[20];
    char sMaxDirSize[20];

    // If settings.cfg exists, update our dlgr_settings.
    // Otherwise, create a settings.cfg with default settings.
    if (access(settingsFile, F_OK | R_OK) == 0){
        settings = fopen(settingsFile, "r");
        if (settings == NULL){
            return ERR_SETTINGS_OPEN;
        }
        if(fgets(sMaxFileSize, 20, (FILE *)settings) == NULL || fgets(sMaxDirSize, 10, (FILE *)settings == NULL)){
            return ERR_SETTINGS_ACCESS;
        }
        dlgr_settings[dlgr_idx].maxFileSize = atoi(sMaxFileSize);
        dlgr_settings[dlgr_idx].maxDirSize = atoi(sMaxDirSize);
        fclose(settings);
    } else {
        settings = fopen(settingsFile, "w");
        if (settings == NULL){
            return ERR_SETTINGS_OPEN;
        }
        fprintf(settings, "8192\n");
        fprintf(settings, "4194304\n");
        fclose(settings);
        sync();
    }

    eprintf("DEBUG: Reached end of initialization.");

    chdir(".."); // Returns up from moduleName folder to log folder.
    chdir(".."); // Returns up from log folder to home directory.

    dlgr_idx++;

    return 1;
}

int dlgr_LogData(char* moduleName, ssize_t size, void *dataIn)
{
    // We can find this module's settings via the following.
    int mod_idx = 0;
    for(; mod_idx < num_systems; mod_idx++){
        if (dlgr_modname[mod_idx] == moduleName){
            break;
        }
    }

    int moduleLogSize = dlgr_settings[mod_idx].moduleLogSize;

    const char directory[] = "log";

    // Change our directory to inside the log folder.
    if (chdir(directory) < 0){
        return ERR_CHDIR_FAIL;
    }

    // Change our directory to inside the moduleName folder.
    if (chdir(moduleName) < 0){
        return ERR_CHDIR_FAIL;
    }

    if (size > moduleLogSize){
        return ERR_MAXLOGSIZE_EXCEEDED;
    }

    // Construct n.dat directory.
    char dataFile[MODULE_FNAME_SZ] = {0x0, };
    snprintf(dataFile, sizeof(dataFile), "%d.dat", logIndex);

    // Construct n+1.dat directory.
    char dataFileNew[MODULE_FNAME_SZ] = {0x0, };
    snprintf(dataFileNew, sizeof(dataFileNew), "%d.dat", logIndex+1);

    // Open the current data (.dat) file in binary-append mode.
    FILE *data = NULL;
    data = fopen(dataFile, "ab");
    if(data == NULL){
        return ERR_DATA_OPEN;
    }

    // Fetch the current file size.
    long int fileSize = ftell(data);

    // If we have to make a new .dat file...
    FILE *index = NULL;

    // Make a new .dat file and iterate the index if necessary.
    if(fileSize >= dlgr_settings[mod_idx].maxFileSize){
        index = fopen(indexFile, "w");
        if (index==NULL){
            return ERR_SETTINGS_OPEN;
        }

        // Iterate the index.
        dlgr_settings[mod_idx].logIndex++;

        // Rewrite the index file.
        fprintf(index, "%d\n", dlgr_settings[mod_idx].logIndex);
        fclose(data);
        fclose(index);
        sync();

        // Switch the data pointer to the new data file.
        data = fopen(dataFileNew, "wb");

        // Delete an old file.
        char dataFileOld[MODULE_FNAME_SZ] = {0x0, };

        eprintf("DEBUG: Deleting old file. This is prone to SEGMENTATION FAULTS.");
        
        snprintf(dataFileOld, sizeof(dataFileOld), "%d.dat", dlgr_settings[mod_idx].logIndex - (dlgr_settings[mod_idx].maxDirSize / dlgr_settings[mod_idx].maxFileSize));
        if (remove(dataFileOld) != 0){
            return ERR_DATA_REMOVE;
        }
        
        eprintf("DEBUG: Deleted old file.");
    }

    // Write FBEGIN, the data, and then FEND to the binary .dat file.
    fwrite(FBEGIN, 6, 1, data);
    fwrite(dataIn, sizeof(dataIn), 1, data);

    // Data padding is size < moduleLogSize.
    for (; size < moduleLogSize; size += sizeof(0)){
        fwrite(dataIn, 1, 1, 0);
    }

    fwrite(FEND, 4, 1, data);

    fclose(data);
    sync();

    eprintf("DEBUG: Finish data log.");

    chdir(".."); // Returns up from moduleName folder to log folder.
    chdir(".."); // Returns up from log folder to home directory.

    return 1;
}

// TODO: Bring RetrieveData and all below functions in line with the current
//       dlgr_settings[] paradigm.

int dlgr_RetrieveData(char *moduleName, char *output, int numRequestedLogs)
{
    eprintf("dlgr_RetrieveData called.");

    /*
     * This should return sets of data from the binary .dat files irregardless of what file its in,
     * for as much as is requested.
     *
     */

    // Assume:      The size of every item within a .dat file is the same size. Note that,
    //              for instance, eps and acs will not have equally sized data.
    // Consider:    A header at the start of the file containing the size of each item.
    // Remember:    .dat binary files look like...

    /* FBEGIN
     * < Some module-specific data of size n >
     * FEND
     */

    int numReadLogs = 0;
    int indexOffset = 0; // How many files back we have to go.
    int errorCheck = 0;

    while (numReadLogs < numRequestedLogs){
        errorCheck = dlgr_retrieve(moduleName, output, numRequestedLogs - numReadLogs, indexOffset);
        if (errorCheck < 0){
            return errorCheck;
        }
        numReadLogs += errorCheck;
        indexOffset++;
    }

    if (numReadLogs != numRequestedLogs){
        return ERR_READ_NUM;
    }

    eprintf("DEBUG: Finished data retrieval.");

    return 1;
}

int dlgr_retrieve(char *moduleName, char *output, int numRequestedLogs, int indexOffset)
{

    // We can find this module's settings via the following.
    int mod_idx = 0;
    for(; mod_idx < num_systems; mod_idx++){
        if (dlgr_modname[mod_idx] == moduleName){
            break;
        }
    }

    int moduleLogSize = dlgr_settings[mod_idx].moduleLogSize;

    const char directory[] = "log";

    // Change our directory to inside the log folder.
    if (chdir(directory) < 0){
        return ERR_CHDIR_FAIL;
    }

    // Change our directory to inside the moduleName folder.
    if (chdir(moduleName) < 0){
        return ERR_CHDIR_FAIL;
    }

    int numReadLogs = 0;

    // First, construct directories.
    char dataFile[MODULE_FNAME_SZ] = {0x0, };
    snprintf(dataFile, sizeof(dataFile, "%d.dat", dlgr_settings[mod_idx].logIndex));

    // Open the .dat file in binary-read mode.
    FILE *data = NULL;
    data = fopen(dataFile, "rb");
    if (data == NULL){
        return ERR_DATA_OPEN;
    }

    // Find the total size of the .dat file.
    fseek(data, 0, SEEK_END);
    ssize_t fileSize = ftell(data);
    fseek(data, 0, SEEK_SET);

    // Create a memory buffer of size fileSize
    char *buffer = NULL;
    buffer = malloc(fileSize + 1);
    if (buffer == NULL){
        return ERR_MALLOC;
    }

    // Read the entire file into memory buffer. One byte at a time for sizeof(buffer) bytes.
    if (fread(buffer, 0x1, fileSize, data) !=){
        return ERR_DATA_READ;
    }

    // Create a cursor to mark our current position in the buffer. Pull it one structure from the back.
    char *bufferCursor = buffer + fileSize;
    bufferCursor -= dlgr_settings[mod_idx].moduleLogSize - FBEGIN_SIZE + FEND_SIZE;

    // If we haven't reached the beginning of the buffer, keep getting structures.
    while (buffer - bufferCursor != 0 && numReadLogs < numRequestedLogs){
        // Copies one structure to the output. Contains delimiters and trace amounts of tree nuts.
        memcpy(output, buffer, moduleLogSize + FBEGIN_SIZE + FEND_SIZE);
        numReadLogs++;
    }

    fclose(data);
    free(buffer);

    chdir("..");
    chdir("..");

    return numReadLogs;
}

ssize_t dlgr_QueryMemorySize(char *moduleName, int numRequestedLogs)
{
    // We can find this module's settings via the following.
    int mod_idx = 0;
    for(; mod_idx < num_systems; mod_idx++){
        if (dlgr_modname[mod_idx] == moduleName){
            break;
        }
    }

    int moduleLogSize = dlgr_settings[mod_idx].moduleLogSize;

    return numRequestedLogs * (moduleLogSize + FBEGIN_SIZE + FEND_SIZE);
}

int dlgr_EditSettings(char *moduleName, int value, int setting)
{
    // We can find this module's settings via the following.
    int mod_idx = 0;
    for(; mod_idx < num_systems; mod_idx++){
        if (dlgr_modname[mod_idx] == moduleName){
            break;
        }
    }

    int moduleLogSize = dlgr_settings[mod_idx].moduleLogSize;

    // Change our directory to inside the log folder.
    if (chdir(directory) < 0){
        return ERR_CHDIR_FAIL;
    }

    // Change our directory to inside the moduleName folder.
    if (chdir(moduleName) < 0){
        return ERR_CHDIR_FAIL;
    }

    switch (setting){
        case MAX_FILE_SIZE:
            if (value > SIZE_FILE_HARDLIMIT || value < 1){
                return ERR_SETTINGS_SET;
            }

            dlgr_settings[mod_idx].maxFileSize = value;
            break;
        case MAX_DIR_SIZE:
            if (value > SIZE_DIR_HARDLIMIT || value < 1){
                return ERR_SETTINGS_SET;
            }

            dlgr_settings[mod_idx].maxDirSize = value;
            break;
        default:
            return ERR_DEFAULT_CASE;
    }

    FILE *settings = NULL;
    settings = fopen("settings.cfg", "w");
    if (settings == NULL) {
        return ERR_SETTINGS_OPEN;
    }

    fprintf(settings, "%d\n", dlgr_settings[mod_idx].maxFileSize);
    fprintf(settings, "%d\n", dlgr_settings[mod_idx].maxDirSize);

    fclose(settings);
    sync();

    chdir("..");
    chdir("..");

    return 1;
}

void dlgr_destroy()
{
    // TODO: Add frees
}