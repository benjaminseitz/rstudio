/*
 * SourceIndex.cpp
 *
 * Copyright (C) 2009-12 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "SourceIndex.hpp"

#include <boost/foreach.hpp>

#include <core/FilePath.hpp>
#include <core/PerformanceTimer.hpp>

#include <core/system/ProcessArgs.hpp>

#include "UnsavedFiles.hpp"

using namespace core ;

namespace session {
namespace modules { 
namespace clang {
namespace libclang {

bool SourceIndex::isTranslationUnit(const core::FilePath& filePath)
{
   std::string ex = filePath.extensionLowerCase();
   return (ex == ".c" || ex == ".cc" || ex == ".cpp" ||
           ex == ".m" || ex == ".mm");
}

SourceIndex::SourceIndex()
   : index_(NULL), pCompilationDatabase_(NULL), verbose_(false)
{
}

SourceIndex::~SourceIndex()
{
   try
   {
      // remove all
      removeAllTranslationUnits();

      // dispose the index
      if (index_ != NULL)
         clang().disposeIndex(index_);
   }
   catch(...)
   {
   }
}

void SourceIndex::initialize(CompilationDatabase* pCompilationDatabase,
                             int verbose)
{
   verbose_ = verbose;
   index_ = clang().createIndex(0, (verbose_ > 0) ? 1 : 0);
   pCompilationDatabase_ = pCompilationDatabase;
}

unsigned SourceIndex::getGlobalOptions() const
{
   return clang().CXIndex_getGlobalOptions(index_);
}

void SourceIndex::setGlobalOptions(unsigned options)
{
   clang().CXIndex_setGlobalOptions(index_, options);
}

void SourceIndex::removeTranslationUnit(const std::string& filename)
{
   TranslationUnits::iterator it = translationUnits_.find(filename);
   if (it != translationUnits_.end())
   {
      clang().disposeTranslationUnit(it->second.tu);
      translationUnits_.erase(it->first);
   }
}

void SourceIndex::removeAllTranslationUnits()
{
   for(TranslationUnits::const_iterator it = translationUnits_.begin();
       it != translationUnits_.end(); ++it)
   {
      clang().disposeTranslationUnit(it->second.tu);
   }

   translationUnits_.clear();
}


void SourceIndex::primeTranslationUnit(const FilePath& filePath)
{
   // if we have no record of this translation unit then do a first pass
   std::string filename = filePath.absolutePath();
   if (translationUnits_.find(filename) == translationUnits_.end())
      getTranslationUnit(filePath);
}

void SourceIndex::reprimeTranslationUnit(const core::FilePath& filePath)
{
   // if we have already indexed this translation unit then re-index it
   std::string filename = filePath.absolutePath();
   if (translationUnits_.find(filename) != translationUnits_.end())
      getTranslationUnit(filePath);
}


TranslationUnit SourceIndex::getTranslationUnit(const FilePath& filePath)
{
   // header files get their own codepath
   if (!SourceIndex::isTranslationUnit(filePath))
      return getHeaderTranslationUnit(filePath);

   boost::scoped_ptr<core::PerformanceTimer> pTimer;
   if (verbose_ > 0)
   {
      std::cerr << "CLANG INDEXING: " << filePath.absolutePath() << std::endl;
      pTimer.reset(new core::PerformanceTimer(filePath.filename()));
   }

   // get the arguments and last write time for this file
   std::vector<std::string> args =
               pCompilationDatabase_->compileArgsForTranslationUnit(filePath);
   std::time_t lastWriteTime = filePath.lastWriteTime();

   // look it up
   std::string filename = filePath.absolutePath();
   TranslationUnits::iterator it = translationUnits_.find(filename);

   // check for various incremental processing scenarios
   if (it != translationUnits_.end())
   {
      // alias record
      StoredTranslationUnit& stored = it->second;

      // already up to date?
      if (args == stored.compileArgs && lastWriteTime == stored.lastWriteTime)
      {
         return TranslationUnit(filename, stored.tu);
      }

      // just needs reparse?
      else if (args == stored.compileArgs)
      {
         int ret = clang().reparseTranslationUnit(
                                stored.tu,
                                unsavedFiles().numUnsavedFiles(),
                                unsavedFiles().unsavedFilesArray(),
                                clang().defaultReparseOptions(stored.tu));

         if (ret == 0)
         {
            // update last write time
            stored.lastWriteTime = lastWriteTime;

            // return it
            return TranslationUnit(filename, stored.tu);
         }
         else
         {
            LOG_ERROR_MESSAGE("Error re-parsing translation unit " + filename);
         }
      }
   }

   // if we got this far then there either was no existing translation
   // unit or we require a full rebuild. in all cases remove any existing
   // translation unit we have
   removeTranslationUnit(filename);

   // add verbose output if requested
   if (verbose_ >= 2)
     args.push_back("-v");

   // get the args in the fashion libclang expects (char**)
   core::system::ProcessArgs argsArray(args);

   // create a new translation unit from the file
   CXTranslationUnit tu = clang().parseTranslationUnit(
                         index_,
                         filename.c_str(),
                         argsArray.args(),
                         argsArray.argCount(),
                         unsavedFiles().unsavedFilesArray(),
                         unsavedFiles().numUnsavedFiles(),
                         clang().defaultEditingTranslationUnitOptions());


   // save and return it if we succeeded
   if (tu != NULL)
   {
      translationUnits_[filename] = StoredTranslationUnit(args,
                                                          lastWriteTime,
                                                          tu);

      return TranslationUnit(filename, tu);
   }
   else
   {
      LOG_ERROR_MESSAGE("Error parsing translation unit " + filename);
      return TranslationUnit();
   }
}

TranslationUnit SourceIndex::getHeaderTranslationUnit(
                                        const core::FilePath& filePath)
{
   // scan through our existing translation units for this file
   for(TranslationUnits::const_iterator it = translationUnits_.begin();
       it != translationUnits_.end(); ++it)
   {
      TranslationUnit tu(it->first, it->second.tu);
      if (tu.includesFile(filePath))
         return tu;
   }

   // drats we don't have it! we can still try to index other src files
   // in search of one that includes this header
   std::vector<core::FilePath> srcFiles =
                                  pCompilationDatabase_->translationUnits();
   BOOST_FOREACH(const FilePath& srcFile, srcFiles)
   {
      TranslationUnit tu = getTranslationUnit(srcFile);
      if (!tu.empty())
      {
         // found it! (keep it in case we need it again)
         if (tu.includesFile(filePath))
         {
            return tu;
         }
         // didn't find it (dispose it to free memory)
         else
         {
            removeTranslationUnit(filePath.absolutePath());
         }
      }
   }

   return TranslationUnit();
}



// singleton
SourceIndex& sourceIndex()
{
   static SourceIndex instance;
   return instance;
}

} // namespace libclang
} // namespace clang
} // namespace modules
} // namesapce session

