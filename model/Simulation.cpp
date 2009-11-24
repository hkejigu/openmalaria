/*
  This file is part of OpenMalaria.
 
  Copyright (C) 2005,2006,2007,2008 Swiss Tropical Institute and Liverpool School Of Tropical Medicine
 
  OpenMalaria is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or (at
  your option) any later version.
 
  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

*/

#include "Simulation.h"

#include "util/BoincWrapper.h"
#include "util/timer.h"
#include "util/gsl.h"
#include "population.h"
#include "Surveys.h"
#include "Drug/DrugInteractions.h"
#include "Global.h"
#include "Transmission/TransmissionModel.h"
#include "inputData.h"
#include <fstream>
#include "gzstream.h"

int Simulation::simPeriodEnd;
int Simulation::totalSimDuration;
int Simulation::simulationTime;
int Simulation::timeStep = TIMESTEP_NEVER;


Simulation::Simulation()
{
  // Initialize input variables and allocate memory.
  // We try to make initialization hierarchical (i.e. most classes initialise
  // through Population::init).
  gsl::setUp();
  Surveys.init();
  Population::init();
  _population = new Population();
  DrugInteractions::init();
}

Simulation::~Simulation(){
  //free memory
  Population::clear();
  
  gsl::tearDown();
}

int Simulation::start(){
  simulationTime = 0;
  _population->estimateRemovalRates();
  if (isCheckpoint()) {
    _population->setupPyramid(true);
    readCheckpoint();
  } else {
    _population->setupPyramid(false);
  }
  
  simPeriodEnd = _population->_transmissionModel->vectorInitDuration();
  // +1 to let final survey run
  totalSimDuration = simPeriodEnd + Global::maxAgeIntervals + Surveys.getFinalTimestep() + 1;
  
  while (simulationTime < simPeriodEnd) {
    vectorInitialisation();
    int extend = _population->_transmissionModel->vectorInitIterate ();
    simPeriodEnd += extend;
    totalSimDuration += extend;
  }
  
  simPeriodEnd += Global::maxAgeIntervals;
  updateOneLifespan();
  
  simPeriodEnd = totalSimDuration;
  mainSimulation();
  return 0;
}

void Simulation::vectorInitialisation () {
  while(simulationTime < simPeriodEnd) {
    ++simulationTime;
    _population->update1();
    
    BoincWrapper::reportProgress (double(simulationTime) / totalSimDuration);
    if (BoincWrapper::timeToCheckpoint()) {
      writeCheckpoint();
      BoincWrapper::checkpointCompleted();
    }
  }
}

void Simulation::updateOneLifespan () {
  int testCheckpointStep = -1;
  if (Global::clOptions & CLO::TEST_CHECKPOINTING)
    testCheckpointStep = simPeriodEnd - Global::maxAgeIntervals / 2;
  while(simulationTime < simPeriodEnd) {
    ++simulationTime;
    _population->update1();
    
    BoincWrapper::reportProgress (double(simulationTime) / totalSimDuration);
    if (BoincWrapper::timeToCheckpoint() || simulationTime == testCheckpointStep) {
      writeCheckpoint();
      BoincWrapper::checkpointCompleted();
      if (Global::clOptions & CLO::TEST_CHECKPOINTING)
	throw cmd_exit ("Checkpoint test: written checkpoint");
    }
  }
}

void Simulation::mainSimulation(){
  //TODO5D
  timeStep=0;
  _population->preMainSimInit();
  _population->newSurvey();	// Only to reset TransmissionModel::innoculationsPerAgeGroup
  Surveys.incrementSurveyPeriod();
  
  while(simulationTime < simPeriodEnd) {
    if (timeStep == Surveys.currentTimestep) {
      _population->newSurvey();
      Surveys.incrementSurveyPeriod();
    }
    _population->implementIntervention(timeStep);
    //Calculate the current progress
    BoincWrapper::reportProgress(double(simulationTime) / totalSimDuration);
    
    ++simulationTime;
    _population->update1();
    ++timeStep;
    //Here would be another place to write checkpoints. But then we need to save state of the surveys/events.
  }
  cout << "timeStep: "<<timeStep << endl;
  delete _population;
  Surveys.writeSummaryArrays();
}


const char* CHECKPOINT = "checkpoint";

bool Simulation::isCheckpoint(){
  ifstream checkpointFile(CHECKPOINT,ios::in);
  // If not open, file doesn't exist (or is inaccessible)
  return checkpointFile.is_open();
}

void Simulation::writeCheckpoint(){
  // We alternate between two checkpoints, in case program is closed while writing.
  const int NUM_CHECKPOINTS = 2;
  
  // Set so that first checkpoint has number 0
  int checkpointNum = NUM_CHECKPOINTS - 1;
  {	// Get checkpoint number, if any
    ifstream checkpointFile;
    checkpointFile.open(CHECKPOINT, fstream::in);
    if(checkpointFile.is_open()){
      checkpointFile >> checkpointNum;
      checkpointFile.close();
    }
  }
  // Get next checkpoint number:
  checkpointNum = (checkpointNum + 1) % NUM_CHECKPOINTS;
  
  gsl::rngSaveState (checkpointNum);
  
  // Open the next checkpoint file for writing:
  ostringstream name;
  name << CHECKPOINT << checkpointNum;
  if (Global::compressCheckpoints) {
    name << ".gz";
    ogzstream out(name.str().c_str(), ios::out | ios::binary);
    write (out);
    out.close();
  } else {
    ofstream out(name.str().c_str(), ios::out | ios::binary);
    write (out);
    out.close();
  }
  
  {	// Indicate which is the latest checkpoint file.
    ofstream checkpointFile;
    checkpointFile.open(CHECKPOINT,ios::out);
    checkpointFile << checkpointNum;
    checkpointFile.close();
  }
}

void Simulation::write (ostream& out) {
  if (out == NULL || !out.good())
    throw new checkpoint_error ("Unable to write to file");
  
  timer::startCheckpoint ();
  out.precision(20);
  out << simulationTime << endl;
  out << timeStep << endl;
  out << simPeriodEnd << endl;
  out << totalSimDuration << endl;
  Population::staticWrite(out);
  _population->write (out);
  DrugInteractions::writeStatic (out);
  timer::stopCheckpoint ();
}

void Simulation::readCheckpoint() {
  int checkpointNum;
  {	// Find out which checkpoint file is current
    ifstream checkpointFile;
    checkpointFile.open(CHECKPOINT, ios::in);
    checkpointFile >> checkpointNum;
    checkpointFile.close();
  }
  // Open the latest file
  ostringstream name;
  name << CHECKPOINT << checkpointNum;	// try uncompressed
  ifstream in(name.str().c_str(), ios::in | ios::binary);
  if (in.good()) {
    read (in);
    in.close();
  } else {
    name << ".gz";				// then compressed
    igzstream in(name.str().c_str(), ios::in | ios::binary);
    if (!in.good())
      throw checkpoint_error ("Unable to read file");
    read (in);
    in.close();
  }
  
  gsl::rngLoadState (checkpointNum);
  cerr << "Loaded checkpoint from: " << name.str() << endl;
  
  // On resume, write a checkpoint so we can tell whether we have identical checkpointed state
  if (Global::clOptions & CLO::TEST_CHECKPOINTING)
    writeCheckpoint();
}

void Simulation::read (istream& in) {
  in >> simulationTime;
  in >> timeStep;
  in >> simPeriodEnd;
  in >> totalSimDuration;
  Population::staticRead(in);
  _population->read(in);
  DrugInteractions::readStatic (in);
  
  // Read trailing white-space (final endl has not yet been read):
  while (in.good() && isspace (in.peek()))
    in.get();
  if (!in.eof()) {	// if anything else is left
    cerr << "Error (checkpointing): not the whole checkpointing file was read;";
    ifstream *ifCP = dynamic_cast<ifstream*> (&in);
    if (ifCP) {
      streampos i = ifCP->tellg();
      ifCP->seekg(0, ios_base::end);
      cerr << ifCP->tellg()-i << " bytes remaining:";
      ifCP->seekg (i);
    } else	// igzstream can't seek
      cerr << " remainder:" << endl;
    cerr << endl << in.rdbuf() << endl;
  }
}