// Project includes
#include <libvbs.h>
#include <auto_array.h>
#include <evlbidebug.h>
#include <regular_expression.h>
#include <dosyscall.h>
#include <mutex_locker.h>
#include <directory_helper_templates.h>
#include <mk6info.h>
#include <ezexcept.h>
#include <hex.h>
#include <threadutil.h>

// Standardized C++ headers
#include <iostream>
#include <map>
#include <set>
#include <list>
#include <algorithm>
#include <string>
#include <cstddef>
#include <cerrno>
#include <cstring>
//#include <csignal>
#include <cstdlib>
#include <limits>    // std::numeric_limits<>

// Old-style *NIX headers
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>    // for thread safety! ;-)

using namespace std;

DECLARE_EZEXCEPT(vbs_except)
DEFINE_EZEXCEPT(vbs_except)

// Get a handle for invalid file descriptor that isn't -1.
// This frees up the negative file descriptors for Mark6 use
// in the filechunk_type below
const int invalidFileDescriptor = std::numeric_limits<int>::max();

/////////////////////////////////////////////////////
//
//  Each chunk detected for a recording
//  gets its own metadata
//
/////////////////////////////////////////////////////
struct filechunk_type {
    // Note: no default c'tor

    // construct from full path name - this is for a FlexBuff chunk
    filechunk_type(string const& fnm):
        pathToChunk( fnm ), chunkPos( 0 ), chunkFd( invalidFileDescriptor ), chunkOffset( 0 )
    {
        // At this point we assume 'fnm' looks like
        // "/path/to/file/chunk.012345678"
        string::size_type   dot = fnm.find_last_of('.');

        if( dot==string::npos )
            throw EINVAL;
            //throw string("error parsing chunk name ")+fnm+": no dot found!";

        // Get the chunk size
        int  fd = ::open( fnm.c_str(), O_RDONLY );
        if( fd<0 )
            throw errno;
            //throw string("error opening ")+fnm+": "+string(evlbi5a::strerror(errno));
        chunkSize = ::lseek(fd, 0, SEEK_END);
        ::close( fd );

        // Note: we must instruct strtoul(3) to use base 10 decoding. The
        // numbers start with a loooot of zeroes mostly and if you pass "0"
        // as third arg to strtoul(3) "accept any base, derive from prefix"
        // this throws off the automatic number-base detection [it would
        // interpret the number as octal].
        chunkNumber = (unsigned int)::strtoul(fnm.substr(dot+1).c_str(), 0, 10);
    }

    // Constructor for a Mark6 format chunk. It has a number, a size, a location
    // within a file and the file descriptor whence it came
    filechunk_type(unsigned int chunk, off_t fpos, off_t sz, int fd):
        chunkSize( sz ), chunkPos( fpos ), chunkFd( -fd ), chunkOffset( 0 ), chunkNumber( chunk )
    {}

    // When copying file chunks be sure to copy the file descriptor only in the Mark6 case.
    // In the FlexBuff case we want to open new file descriptor for this chunk; each flexbuff 
    // file chunk manages its own file descriptor.
    filechunk_type(filechunk_type const& other):
        pathToChunk( other.pathToChunk ), chunkSize( other.chunkSize ), chunkPos( other.chunkPos ), 
        chunkFd( (other.chunkFd<0) ? other.chunkFd : invalidFileDescriptor ),
        chunkOffset( other.chunkOffset ), chunkNumber( other.chunkNumber )
    { }

    int open_chunk( void ) const {
        errno = 0;
        if( chunkFd==invalidFileDescriptor ) {
            if( (chunkFd=::open(pathToChunk.c_str(), O_RDONLY))==-1 )
                chunkFd = invalidFileDescriptor;
            DEBUG(5, "filechunk_type:open_chunk[" << pathToChunk << "] fd#" << chunkFd << " " << evlbi5a::strerror(errno) << endl);
        }
        return (chunkFd<0) ? -chunkFd : chunkFd;
    }

    void close_chunk( void ) const {
        // return to initial state in case of non-Mark6 file descriptor
        if( chunkFd>=0 && chunkFd!=invalidFileDescriptor ) {
            ::close(chunkFd);
            DEBUG(5, "filechunk_type:close_chunk[" << pathToChunk << "] fd#" << chunkFd << endl);
            chunkFd  = invalidFileDescriptor;
        }
        return;
    }

    ~filechunk_type() {
        // don't leak file descriptors
        if( chunkFd>=0 && chunkFd!=invalidFileDescriptor ) {
            ::close( chunkFd );
            DEBUG(5, "filechunk_type:~filechunk_type[" << pathToChunk << "] close fd#" << chunkFd << endl);
        }
    }

    // Note: declare chunkOffset as mutable such that we can later, after
    // the chunks have been sorted and put into a set, update the
    // chunkOffset value to what it should be. [Elements in a set are const
    // in order to guarantee that their sorting order is not compromised
    // as you alter the element - in this case WE know that the sorting only
    // depends on the value of 'chunkNumber' so we can safely edit
    // chunkOffset w/o worrying about compromising the set]
    string                 pathToChunk;
    off_t                  chunkSize;
    off_t                  chunkPos;
    mutable int            chunkFd;
    mutable off_t          chunkOffset;
    unsigned int           chunkNumber;

    private:
        // no default c'tor!
        filechunk_type();
};

// note that we let the filechunks be an automatically sorted container
// Comparison operator for filechunk_type - sort by chunkNumber exclusively!
//
bool operator<(filechunk_type const& l, filechunk_type const& r) {
    return l.chunkNumber < r.chunkNumber;
}

typedef set<filechunk_type>             filechunks_type;


////////////////////////////////////////////////////////////////
//
//  Prototypes so we can use the calls; implementation is at the
//  bottom of this module
//
////////////////////////////////////////////////////////////////

// These look for VBS recordings
void scanRecording(string const& recname, direntries_type const& mountpoints, filechunks_type& fcs);
void scanRecordingMountpoint(string const& recname, string const& mp, filechunks_type& fcs);
void scanRecordingDirectory(string const& recname, string const& dir, filechunks_type& fcs);

// These for Mark6
void scanMk6Recording(string const& recname, direntries_type const& mountpoints, filechunks_type& fcs);
void scanMk6RecordingMountpoint(string const& recname, string const& mountpoint, filechunks_type& fcs);
void scanMk6RecordingFile(string const& recname, string const& file, filechunks_type& fcs);

////////////////////////////////////////
//
//  isMountpoint:
//
//  functor predicate, returns true
//  if directory entry is named
//  "disk[0-9]+" and is a directory
//
/////////////////////////////////////////

struct isMountpoint {
    bool operator()(string const& entry) const {
        Regular_Expression      rxDisk("^disk[0-9]{1,}$");
        struct stat             status;
        string::size_type       slash = entry.find_last_of("/");

        // IF there is a slash, we skip it, if there isn't, we
        // use the whole string
        if( slash==string::npos )
            slash = 0;
        else
            slash += 1;
        DEBUG(5, "isMountpoint: checking name " << entry.substr(slash) << endl);
        if( !rxDisk.matches(entry.substr(slash)) )
            return false;
        if( ::lstat(entry.c_str(), &status)<0 ) {
            DEBUG(4, "predMountpoint: ::lstat fails on " << entry << " - " << evlbi5a::strerror(errno) << endl);
            return false;
        }
        // We must have r,x access to the directory [in order to descend into it]
        return S_ISDIR(status.st_mode) && (status.st_mode & S_IRUSR) && (status.st_mode & S_IXUSR);
    }
};



///////////////////////////////////////////////////////////
//
//      Mapping of filedescriptor to open file
//      shared data structure -> must be thread safe
//
// ////////////////////////////////////////////////////////
struct openfile_type {
    off_t                           filePointer;
    off_t                           fileSize;
    filechunks_type                 fileChunks;
    filechunks_type::iterator       chunkPtr;

    // No default c'tor!
    openfile_type(filechunks_type const& fcs):
        filePointer( 0 ), fileSize( 0 ), fileChunks( fcs )
    {
        for(chunkPtr=fileChunks.begin(); chunkPtr!=fileChunks.end(); chunkPtr++) {
            // Offset is recording size counted so far
            chunkPtr->chunkOffset = fileSize;
            // And add the current chunk to the recording size
            fileSize += chunkPtr->chunkSize;
        }
        chunkPtr = fileChunks.begin();
        DEBUG(2, "openfile_type: found " << fileSize << " bytes in " << fileChunks.size() << " chunks, " <<
                 (((double)fileChunks.size())/((double)fileChunks.rbegin()->chunkNumber+1))*100.0 << "%" << endl);
    }

    // The copy c'tor must take care of initializing the filechunk iterator
    // to point it its own filechunks, not at the other guys'
    openfile_type(openfile_type const& other):
        filePointer( 0 ), fileSize( other.fileSize ),
        fileChunks( other.fileChunks ), chunkPtr( fileChunks.begin() )
    {}

    ~openfile_type() {
        // unobserve all chunks
        for( chunkPtr=fileChunks.begin(); chunkPtr!=fileChunks.end(); chunkPtr++)
            chunkPtr->close_chunk();
    }
    private:
        openfile_type();
};

typedef map<int, openfile_type>         openedfiles_type;

pthread_rwlock_t                        openedFilesLock = PTHREAD_RWLOCK_INITIALIZER;
openedfiles_type                        openedFiles;


////////////////////////////////////////////////////////
//
// Upon shutdown, the library will close all open files
//
// /////////////////////////////////////////////////////
struct cleanup_type {
    cleanup_type() {
        // here we could do initialization
    }

    ~cleanup_type() {
        // Before clearing the caches, do clear the open files
        // Actually, that is taken care of by the openfile d'tor
        openedFiles.clear();
    }
};
static cleanup_type                     cleanup = cleanup_type();


#if 0
//////////////////////////////////////////
//
//  int vbs_open()
//
//  Verify that the current root dir
//  exists and that we have sufficient
//  privileges to look inside it.
//
//  Return 0 on success, -1 on error and
//  sets errno.
//
/////////////////////////////////////////
int vbs_open(char const* recname, char const* const rootdir ) {
    if( recname==0 || *recname==0 || rootdir==0 || *rootdir=='\0' ) {
        errno = EINVAL;
        return -1;
    }

    // Test if rootdir is a sensible dir & find all
    // flexbuf mountpoints there
    struct stat status;

    // Propagate failure to stat the rootdir 
    if( ::lstat(rootdir, &status)<0 )
        return -1;

    // Verify that it is a DIR that we have permission to enter into and
    // read
    if( !S_ISDIR(status.st_mode) ) {
        errno = ENOTDIR;
        return -1;
    }
    if( (status.st_mode & S_IRUSR)==0 ||
        (status.st_mode & S_IXUSR)==0 ) {
            errno = EPERM;
            return -1;
    }

    // scan rootdir for mountpoints
    try {
        direntries_type mps = dir_filter(string(rootdir), isMountpoint());

        if( mps.empty() ) {
            // No mountpoints?
            errno = ENOENT;
            return -1;
        }

        // Now that we have mountpoints, we can scan those for 
        // the recording chunks - basically we now fall into
        // vbs_open2() as we have an array of mountpoints to scan
        auto_array<char const*>         vbsdirs( new char const*[ mps.size()+1 ] );
        direntries_type::const_iterator curmp = mps.begin();

        // Get array of "char*" and add a terminating 0 pointer
        for(unsigned int i=0; i<mps.size(); i++, curmp++)
            vbsdirs[i] = curmp->c_str();
        vbsdirs[ mps.size() ] = 0;

        return ::vbs_open2(recname, &vbsdirs[0]);
    }
    catch( int eno ) {
        errno = eno;
        return -1;
    }
    errno = EINVAL;
    return -1;
}
#endif

/////////////////////////////////////////////////////////////////////
//
// vbs_open2(recname, char const* const* rootdirs)
//
// Assume 'rootdirs' is an array of mountpoints. Scan each mountpoint
// for chunks of the recording by the name 'recname'
//
////////////////////////////////////////////////////////////////////
int vbs_open( char const* recname, char const* const* rootdirs ) {
    // Sanity checks
    if( recname==0 || *recname=='\0' || rootdirs==0 ) {
        errno = EINVAL;
        return -1;
    }

    // Assume all of the entries in the rootdirs ARE mountpoints
    direntries_type  newmps;

    for( ; *rootdirs; rootdirs++)
        newmps.insert( string(*rootdirs) );

    // Ok, scan all mountpoints for chunks of the recording
    filechunks_type  chunks;
 
    // The dir_mapper/dir_filter functions from directory_helper_templates.h
    // throw the error code if they fail. Here we translate that into
    // a better typed exception 
    try { 
        ::scanRecording(recname, newmps, chunks);
    } catch( int eno ) {
        throw syscallexception( std::string("vbs_open '")+recname+"' fails - "+std::string(evlbi5a::strerror(eno)));
    }

    if( chunks.empty() ) {
        // nothing found?
        errno = ENOENT;
        return -1;
    }

    // Rite! We must allocate a new file descriptor!
    rw_write_locker lockert( openedFilesLock );

    const int fd = (openedFiles.empty() ? std::numeric_limits<int>::max() : (openedFiles.begin()->first - 1));
    openedFiles.insert( make_pair(fd, openfile_type(chunks)) );
    return fd;
}

/////////////////////////////////////////////////////////////////////
//
// mk6_open(recname, char const* const* rootdirs)
//
// Assume 'rootdirs' is an array of mountpoints. Scan each mountpoint
// for the files called 'recname', check if they're Mark6 files
// and analyze them
//
////////////////////////////////////////////////////////////////////
int mk6_open( char const* recname, char const* const* rootdirs ) {
    // Sanity checks
    if( recname==0 || *recname=='\0' || rootdirs==0 ) {
        errno = EINVAL;
        return -1;
    }

    // Assume all of the entries in the rootdirs ARE mountpoints
    direntries_type  newmps;

    for( ; *rootdirs; rootdirs++)
        newmps.insert( string(*rootdirs) );

    // Ok, scan all mountpoints for chunks of the recording
    filechunks_type  chunks;
   
    ::scanMk6Recording(recname, newmps, chunks);

    if( chunks.empty() ) {
        // nothing found?
        errno = ENOENT;
        return -1;
    }

    // Rite! We must allocate a new file descriptor!
    rw_write_locker lockert( openedFilesLock );

    const int fd = (openedFiles.empty() ? std::numeric_limits<int>::max()  : (openedFiles.begin()->first - 1));
    openedFiles.insert( make_pair(fd, openfile_type(chunks)) );
    return fd;
}


//////////////////////////////////////////////////
//
//  int vbs_read(int fd, void* buf, size_t count)
//
//  read bytes from a previously opened recording
//
//////////////////////////////////////////////////

ssize_t vbs_read(int fd, void* buf, size_t count) {
    // we need read-only access to the int -> openfile_type mapping
    rw_read_locker             lockert( openedFilesLock );
    unsigned char*             bufc = (unsigned char*)buf;
    openedfiles_type::iterator fptr = openedFiles.find(fd) ;

    if( fptr==openedFiles.end() ) {
        errno = EBADF;
        return -1;
    }
    if( buf==0 ) {
        errno = EFAULT;
        return -1;
    }

    // when reading zero bytes, we're done. We even have done
    // a basic level of error checking ... (according to POSIX
    // that's ok)
    // http://pubs.opengroup.org/onlinepubs/009695399/functions/read.html
    if( count==0 )
        return 0;

    // Read bytes from file!
    int              realfd;
    size_t           nr = count;
    openfile_type&   of = fptr->second;
    filechunks_type& chunks = of.fileChunks;

    // Cant read past eof
    if( of.chunkPtr==chunks.end() )
        return 0;

    // While we need to read bytes
    while( nr ) {
        // If we hit eof whilst reading that's not an error but we'd better
        // stop reading
        if( of.chunkPtr==chunks.end() )
            break;

        // Ok, we might be adressing inside a valid chunk
        const filechunk_type& chunk = *of.chunkPtr;

        // How much bytes can we read?
        off_t   n2r = min((off_t)nr, chunk.chunkOffset+chunk.chunkSize - of.filePointer);
        ssize_t actualread;

        if( n2r<=0 ) {
            // None at all, apparently. Move to next block!
            chunk.close_chunk();
            if( of.chunkPtr!=chunks.end() )
                of.chunkPtr++;
            continue;
        }

        // If we cannot open the current chunk
        if( (realfd=chunk.open_chunk())==invalidFileDescriptor )
            break;

        // Ok. Seek into the realfd
        ::lseek(realfd, of.filePointer - chunk.chunkOffset + chunk.chunkPos, SEEK_SET);

        // And read them dang bytes!
        if( (actualread=::read(realfd, bufc, (size_t)n2r))<0 )
            break;

        // Update pointers
        bufc           += actualread;
        nr             -= actualread;
        of.filePointer += actualread;
    }
    return (ssize_t)(count-nr);
}

//////////////////////////////////////////////////
//
//  int vbs_lseek(int fd, off_t offset, int whence)
//
//  see lseek(2)
//
/////////////////////////////////////////////////

off_t vbs_lseek(int fd, off_t offset, int whence) {
    // we need read-only access to the int -> openfile_type mapping
    rw_read_locker             lockert( openedFilesLock );
    off_t                      newfp;
    openedfiles_type::iterator fptr  = openedFiles.find(fd);

    if( fptr==openedFiles.end() ) {
        errno = EBADF;
        return -1;
    }

    openfile_type&  of   = fptr->second;

    switch( whence ) {
        case SEEK_SET:
            newfp = offset;
            break;
        case SEEK_END:
            newfp = of.fileSize + offset;
            break;
        case SEEK_CUR:
            newfp = of.filePointer + offset;
            break;
        default:
            errno = EINVAL;
            return (off_t)-1;
    }
    if( newfp<0 ) {
        errno = EINVAL;
        return (off_t)-1;
    }
    // If the new file pointer is equal to the current file pointer,
    // we're done very quickly ...
    if( newfp==of.filePointer )
        return of.filePointer;

    // We've got the new file pointer!
    // Now skip to the chunk what contains the pointer
    filechunks_type::iterator newchunk   = of.fileChunks.begin();
    
    while( newchunk!=of.fileChunks.end() && newfp>(newchunk->chunkOffset+newchunk->chunkSize) )
        newchunk++;

    // unobserve current chunk if new chunk is different
    if( of.chunkPtr!=newchunk && of.chunkPtr!=of.fileChunks.end() )
        of.chunkPtr->close_chunk();

    // Ok, update open file status
    of.filePointer = newfp;
    of.chunkPtr    = newchunk;

    return of.filePointer;
}

//////////////////////////////////
//
//  int vbs_close(int fd)
//
//  close a previously opened
//  recording
//
//////////////////////////////////
typedef set<int> fdset_type;

int vbs_close(int fd) {
    // we need write access to the int -> openfile_type mapping
    rw_write_locker            lockert( openedFilesLock );
    openedfiles_type::iterator fptr = openedFiles.find(fd);

    if( fptr==openedFiles.end() ) {
        errno = EBADF;
        return -1;
    }
    // Before erasing, we must take care of potential Mark6 file descriptors
    fdset_type  mk6fds;

    for(filechunks_type::const_iterator p=fptr->second.fileChunks.begin(); p!=fptr->second.fileChunks.end(); p++)
        if( p->chunkFd<0 )
            mk6fds.insert( p->chunkFd );
    // The openfile_type d'tor will close lingering FlexBuff file
    // descriptors
    openedFiles.erase( fptr );
    // We manually close the Mark6 files
    for(fdset_type::iterator p=mk6fds.begin(); p!=mk6fds.end(); p++) {
        DEBUG(5, "vbs_close: closing Mark6 fd#" << -*p << endl);
        ::close( -*p );
    }
    return 0;
}

#if 0
//////////////////////////////////////////
//
//  int  vbs_setdbg(int newlevel)
//
//  Set a new debug level. Higher positive
//  numbers yield more detailed output.
//
//  Always succeeds and returns the 
//  previous debug level
//
/////////////////////////////////////////
int vbs_setdbg(int newlevel) {
    int   rv = dbglev_fn( newlevel );
    // prefent function signatures to be printed - always
    fnthres_fn( newlevel+1 );
    return rv;
}
#endif




////////////////////////////////////////
//
//  scan mountpoints for the requested
//  FlexBuff style recording
//
/////////////////////////////////////////

void scanRecording(string const& recname, direntries_type const& mountpoints, filechunks_type& fcs) {
    // Loop over all mountpoints and check if there are file chunks for this
    // recording
    for(direntries_type::const_iterator curmp=mountpoints.begin(); curmp!=mountpoints.end(); curmp++)
        scanRecordingMountpoint(recname, *curmp, fcs);
}

void scanRecordingMountpoint(string const& recname, string const& mp, filechunks_type& fcs) {
    struct stat     dirstat;
    const string    dir(mp+"/"+recname);

    if( ::lstat(dir.c_str(), &dirstat)<0 ) {
        if( errno!=ENOENT )
            DEBUG(4, "scanRecordingMountpoint(" << recname << ", " << mp << ")/::lstat() fails - " << evlbi5a::strerror(errno) << endl);
        return;
    }
    // OK, we got the status. If it's not a directory ...
    if( !S_ISDIR(dirstat.st_mode) )
        return;

    // Go ahead and scan the directory for chunks
    scanRecordingDirectory(recname, dir, fcs);
}

// Complaint by users: jive5ab, m5copy, vbs_ls, vbs_rm and vbs_fs don't seem to pick up
// FlexBuff recordings with regex majik characters (".", "+" et.al.) in
// them. Now, creating recordings with those characters in their names might
// be capital offence in the first place ... but since no-one's stopping
// them it might be considered polite to suck it up and make the s/w operate
// correctly just the same.
// In vbs_ls/vbs_rm the issue was fixed by escaping the recording name
// before making a regex pattern out of it for the VBS shrapnel:
//      https://docs.python.org/2/library/re.html#re.escape
//
// I've looked up the implementation of re.escape:
//      https://github.com/python/cpython/blob/master/Lib/re.py#L249
//      (status as on 09 Feb 2017)
//
// and I think it's easy enough to emulate that here in C++
static const string alphanum_str("_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890");

string escape(string const& s) {
    string                       rv;
    back_insert_iterator<string> output(rv);

    for(string::const_iterator p=s.begin(); p!=s.end(); *output++ = *p++)
        if( alphanum_str.find(*p)==string::npos )
            *output++ = '\\';
    return rv;
}

struct isRecordingChunk {
    isRecordingChunk(string const& recname):
        __m_regex( string("^")+escape(recname)+"\\.[0-9]{8}$" )
    {}

    bool operator()(string const& entry) const {
        DEBUG(5, "checking entry " << entry << " against " << __m_regex.pattern() << endl);
        return __m_regex.matches(entry);
    }

    Regular_Expression  __m_regex;

    private:
        isRecordingChunk();
        isRecordingChunk( isRecordingChunk const& );
};

void scanRecordingDirectory(string const& recname, string const& dir, filechunks_type& rv) {
    DIR*             dirp;
    direntries_type  chunks;
    isRecordingChunk predicate( recname );

    if( (dirp=::opendir(dir.c_str()))==0 ) {
        DEBUG(4, "scanRecordingDirectory(" << recname << ", " << dir << ")/ ::opendir fails - " << evlbi5a::strerror(errno) << endl);
        return;
    }
    chunks = dir_filter(dirp, predicate);
    ::closedir(dirp);

    // If we find duplicates, now *that* is a reason to throw up
    for(direntries_type::const_iterator p=chunks.begin(); p!=chunks.end(); p++)
        EZASSERT2((rv.insert(filechunk_type(dir+"/"+*p))).second, vbs_except, EZINFO(" duplicate insert for chunk " << *p));
}

////////////////////////////////////////
//
//  scan mountpoints for the requested
//  Mark6 recording
//
/////////////////////////////////////////

struct sm6mp_args {
    string           recname;
    string           mp;
    filechunks_type* fcsptr;
    pthread_mutex_t* mtx;

    sm6mp_args(string const& recnam, string const& mountpoint, filechunks_type* fcs, pthread_mutex_t* ptmtx):
        recname( recnam ), mp( mountpoint ), fcsptr( fcs ), mtx( ptmtx )
    {}
};

void* scanMk6RecordingMountpoint_thrd(void* args) {
    sm6mp_args*     sm6mp = (sm6mp_args*)args;
    struct stat     filestat;
    const string    file(sm6mp->mp+"/"+sm6mp->recname);

    if( ::lstat(file.c_str(), &filestat)<0 ) {
        if( errno!=ENOENT )
            DEBUG(4, "scanMk6RecordingMountpoint(" << sm6mp->recname << ", " << sm6mp->mp << ")/::lstat() fails - " << evlbi5a::strerror(errno) << endl);
        delete sm6mp;
        return (void*)0;
    }
    // OK, we got the status. If it's not a regular file ...
    if( !S_ISREG(filestat.st_mode) ) {
        delete sm6mp;
        return (void*)0;
    }

    // Go ahead and scan the file for chunks
    // We first build a local filechunks thing. When complete, then we lock
    // and copy our findins into the global one
    filechunks_type  lcl;
    scanMk6RecordingFile(sm6mp->recname, file, lcl);
    ::pthread_mutex_lock(sm6mp->mtx);
    for(filechunks_type::const_iterator curfc=lcl.begin(); curfc!=lcl.end(); curfc++)
        if( (sm6mp->fcsptr->insert( *curfc )).second==false )
            DEBUG(-1, "scanMkRecordingMountpoint: duplicate file chunk " << curfc->chunkNumber << " found in " << file << endl);
    ::pthread_mutex_unlock(sm6mp->mtx);
    delete sm6mp;
    return (void*)0;
}

typedef std::list<pthread_t*>               threadlist_type;
void scanMk6Recording(string const& recname, direntries_type const& mountpoints, filechunks_type& fcs) {
    // Loop over all mountpoints and check if there are file chunks for this
    // recording
    // HV: 04 Nov 2015 Do the scan multithreaded - one thread per mountpoint
    threadlist_type threads;
    pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

    for(direntries_type::const_iterator curmp=mountpoints.begin(); curmp!=mountpoints.end(); curmp++) {
        int         create_error = 0;
        pthread_t*  tidptr = new pthread_t;
        sm6mp_args* sm6mp  = new sm6mp_args(recname, *curmp, &fcs, &mtx);

        if( (create_error=mp_pthread_create(tidptr, &scanMk6RecordingMountpoint_thrd, sm6mp))!=0 ) {
            DEBUG(-1, "scanMk6Recording: failed to create thread [" << *curmp << "] - " << evlbi5a::strerror(create_error) << endl);
            delete tidptr;
            delete sm6mp;
            break;
        }
        threads.push_back( tidptr );
    }
    // Wait for completion of threads that have succesfully started
    for(threadlist_type::iterator tidptrptr=threads.begin(); tidptrptr!=threads.end(); tidptrptr++) {
        ::pthread_join( **tidptrptr, 0 );
        delete *tidptrptr;
    }
}

// Note! Upon succesfull exit, we do NOT close 'fd'!
// So the caller must make sure to always account for
// the file descriptors!
#define MYMAX_Local(a, b) ((a>b)?(a):(b))
void scanMk6RecordingFile(string const& /*recname*/, string const& file, filechunks_type& rv) {
    int               fd;
    off_t             fpos;
    const size_t      fh_size = sizeof(mk6_file_header);
    const size_t      wb_size = sizeof(mk6_wb_header_v2);
    unsigned char     buf[ fh_size+wb_size /*MYMAX_Local(fh_size, wb_size)*/ ];
    mk6_file_header*  fh6  = (mk6_file_header*)&buf[0];
    mk6_wb_header_v2* wbh  = (mk6_wb_header_v2*)&buf[0];

    // File existence has been checked before so now we MUST be able to open it
    ASSERT2_POS( fd=::open(file.c_str(), O_RDONLY), SCINFO(" failed to open file " << file) );

    // It may well not be a Mk6 recording, for all we know
    if( ::read(fd, fh6, sizeof(mk6_file_header))!=sizeof(mk6_file_header) ) {
        DEBUG(4, "scanMk6RecordingFile[" << file << "]: fail to read mk6 header - " << evlbi5a::strerror(errno) << endl);
        ::close(fd);
        return;
    }

    if( fh6->sync_word!=MARK6_SG_SYNC_WORD ) {
        DEBUG(4, "scanMk6RecordingFile[" << file << "]: did not find mk6 sync word in header" << endl);
        ::close(fd);
        return;
    }
    if( fh6->version!=2 ) {
        DEBUG(4, "scanMk6RecordingFile[" << file << "]: we don't support mk6 file version " << fh6->version << endl);
        ::close(fd);
        return;
    }
    DEBUG(4, "scanMk6RecordingFile[" << file << "]: starting" << endl);
    // Ok. Now we should just read all the blocks in this file!
    fpos = fh_size;
    while( ::read(fd, wbh, wb_size)==(ssize_t)wb_size ) {
        // Don't forget that the block sizes written in the Mark6 files are
        // including the write-block-header size! (Guess how I found out
        // that I'd forgotten just that ...)

        // Make sure there's sense in the block number and size, otherwise better give up
        EZASSERT2(wbh->blocknum>=0 && wbh->wb_size>0, vbs_except,
                  EZINFO(" found bogus stuff in write block header @" << fpos << " in " << file <<
                         ", block# " << wbh->blocknum << ", sz=" << wbh->wb_size);
                  ::close(fd));

        // Ok, found another block!
        fpos += wb_size;

        // We cannot tolerate duplicate inserts
        EZASSERT2(rv.insert(filechunk_type((unsigned int)wbh->blocknum, fpos, wbh->wb_size-wb_size, fd)).second, vbs_except,
                  EZINFO(" duplicate insert for chunk " << wbh->blocknum); ::close(fd) );

        // Advance file pointer
        fpos += (wbh->wb_size - wb_size);
        if( ::lseek(fd, fpos, SEEK_SET)==(off_t)-1 ) {
            DEBUG(4, "scanMk6RecordingFile[" << file << "]: failed to seek to next block @" <<
                     fpos << " - " << evlbi5a::strerror(errno) << endl);
            break;
        }
    }
    DEBUG(4, "scanMk6RecordingFile[" << file << "]: done" << endl);
}
