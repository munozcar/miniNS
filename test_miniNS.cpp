// To compile: clang++ -o test_miniNS test_miniNS.cpp -L./build/ -I ./include/ -l./minins -stdlib=libc++ -std=c++11 -Wno-deprecated-register


#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <Eigen/Dense>
#include <stdio.h>
#include <stdlib.h>
#include "miniNS.h"

// -------------- "LINEAR" FORWARD MODEL ----------------------------------------------------------
class LinearModel : public Model
{
    public:
        LinearModel(const RefArrayXd covariates);
        ~LinearModel();
        ArrayXd getCovariates();
        virtual void predict(RefArrayXd predictions, const RefArrayXd modelParameters);

    protected:
    private:
};

LinearModel::LinearModel(const RefArrayXd covariates)
: Model(covariates)
{
}
LinearModel::~LinearModel()
{
}

void LinearModel::predict(RefArrayXd predictions, RefArrayXd const modelParameters)
{
    double slope = modelParameters(0);
    double offset = modelParameters(1);
    predictions = slope*covariates + offset;
}


// -------------- "PSG RETRIEVAL" FORWARD MODEL ---------------------------------------------------

std::string exec(const char* cmd) {
    char buffer[128];
    std::string result = "";
    FILE* pipe = popen(cmd, "r");
    if (!pipe) throw std::runtime_error("popen() failed!");
    try {
        while (fgets(buffer, sizeof buffer, pipe) != NULL) {
            result += buffer;
        }
    } catch (...) {
        pclose(pipe);
        throw;
    }
    pclose(pipe);
    return result;
}

class PSGModel : public Model
{
    public:
        PSGModel(const RefArrayXd covariates);
        ~PSGModel();
        ArrayXd getCovariates();
        virtual void predict(RefArrayXd predictions, const RefArrayXd modelParameters);

    protected:
    private:
};

void PSGModel::predict(RefArrayXd predictions, RefArrayXd const modelParameters)
{
    std::string out = "";
    double planet_radius = modelParameters(0);
    double planet_temperature = modelParameters(1);
    exec("curl -d type=rad -d whdr=n --data-urlencode file@config.txt https://psg.gsfc.nasa.gov/api.php");
}

// -------------- MAIN PROGRAM --------------------------------------------------------------------

int main(int argc, char *argv[])
{

    // Check number of arguments for main function
    if (argc != 1)
    {
        cerr << "Usage: ./miniNS" << endl;
        exit(EXIT_FAILURE);
    }
    // Read input data
    unsigned long Nrows;
    int Ncols;
    ArrayXXd data;
    string baseInputDirName = "";
    string inputFileName = "input_data.txt"; // data
    string outputPathPrefix = "Inference_";

    ifstream inputFile;
    File::openInputFile(inputFile, inputFileName);
    File::sniffFile(inputFile, Nrows, Ncols);
    data = File::arrayXXdFromFile(inputFile, Nrows, Ncols);
    inputFile.close();

    // Create arrays for each data type
    ArrayXd covariates = data.col(0);
    ArrayXd observations = data.col(1);
    ArrayXd uncertainties = data.col(2);

    // Uniform Prior
    unsigned long Nparameters;  // Number of parameters for which prior distributions are defined

    int Ndimensions = 2;        // Number of free parameters (dimensions) of the problem
    vector<Prior*> ptrPriors(1);
    ArrayXd parametersMinima(Ndimensions);
    ArrayXd parametersMaxima(Ndimensions);
    parametersMinima <<  0.5, 2.0;         // Minima values for the free parameters
    parametersMaxima << 3.0, 20.0;     // Maxima values for the free parameters

    UniformPrior uniformPrior(parametersMinima, parametersMaxima);
    ptrPriors[0] = &uniformPrior;

    string fullPathHyperParameters = outputPathPrefix + "hyperParametersUniform.txt";       // Print prior hyper parameters as output
    uniformPrior.writeHyperParametersToFile(fullPathHyperParameters);

    // Set up the models for the inference problem
    LinearModel model(covariates);      // Linear function of the type f = a*x + b


    // Set up the likelihood function to be used
    NormalLikelihood likelihood(observations, uncertainties, model);

    // Set up the K-means clusterer using an Euclidean metric

    inputFileName = "Xmeans_configuringParameters.txt";
    File::openInputFile(inputFile, inputFileName);
    File::sniffFile(inputFile, Nparameters, Ncols);

    if (Nparameters != 2)
    {
        cerr << "Wrong number of input parameters for X-means algorithm." << endl;
        exit(EXIT_FAILURE);
    }

    ArrayXd configuringParameters;
    configuringParameters = File::arrayXXdFromFile(inputFile, Nparameters, Ncols);
    inputFile.close();

    int minNclusters = configuringParameters(0);
    int maxNclusters = configuringParameters(1);

    if ((minNclusters <= 0) || (maxNclusters <= 0) || (maxNclusters < minNclusters))
    {
        cerr << "Minimum or maximum number of clusters cannot be <= 0, and " << endl;
        cerr << "minimum number of clusters cannot be larger than maximum number of clusters." << endl;
        exit(EXIT_FAILURE);
    }

    int Ntrials = 10;
    double relTolerance = 0.01;

    EuclideanMetric myMetric;
    KmeansClusterer kmeans(myMetric, minNclusters, maxNclusters, Ntrials, relTolerance);

    // Configure and start nested sampling inference -----

    inputFileName = "NSMC_configuringParameters.txt";
    File::openInputFile(inputFile, inputFileName);
    File::sniffFile(inputFile, Nparameters, Ncols);
    configuringParameters.setZero();
    configuringParameters = File::arrayXXdFromFile(inputFile, Nparameters, Ncols);
    inputFile.close();

    if (Nparameters != 8)
    {
        cerr << "Wrong number of input parameters for NSMC algorithm." << endl;
        exit(EXIT_FAILURE);
    }

    bool printOnTheScreen = true;                       // Print results on the screen
    int initialNobjects = configuringParameters(0);     // Initial number of live points
    int minNobjects = configuringParameters(1);         // Minimum number of live points
    int maxNdrawAttempts = configuringParameters(2);    // Maximum number of attempts when trying to draw a new sampling point
    int NinitialIterationsWithoutClustering = configuringParameters(3); // The first N iterations, we assume that there is only 1 cluster
    int NiterationsWithSameClustering = configuringParameters(4);       // Clustering is only happening every N iterations.
    double initialEnlargementFraction = configuringParameters(5);   // Fraction by which each axis in an ellipsoid has to be enlarged.
                                                                    // It can be a number >= 0, where 0 means no enlargement.
    double shrinkingRate = configuringParameters(6);        // Exponent for remaining prior mass in ellipsoid enlargement fraction.
                                                            // It is a number between 0 and 1. The smaller the slower the shrinkage
                                                            // of the ellipsoids.

    if ((shrinkingRate > 1) || (shrinkingRate) < 0)
    {
        cerr << "Shrinking Rate for ellipsoids must be in the range [0, 1]. " << endl;
        exit(EXIT_FAILURE);
    }

    double terminationFactor = configuringParameters(7);    // Termination factor for nested sampling process.


    MultiEllipsoidSampler nestedSampler(printOnTheScreen, ptrPriors, likelihood, myMetric, kmeans,
                                        initialNobjects, minNobjects, initialEnlargementFraction, shrinkingRate);

    double tolerance = 1.e2;
    double exponent = 0.4;
    PowerlawReducer livePointsReducer(nestedSampler, tolerance, exponent, terminationFactor);

    nestedSampler.run(livePointsReducer, NinitialIterationsWithoutClustering, NiterationsWithSameClustering,
                      maxNdrawAttempts, terminationFactor, outputPathPrefix);

    nestedSampler.outputFile << "# List of configuring parameters used for the ellipsoidal sampler and X-means" << endl;
    nestedSampler.outputFile << "# Row #1: Minimum Nclusters" << endl;
    nestedSampler.outputFile << "# Row #2: Maximum Nclusters" << endl;
    nestedSampler.outputFile << "# Row #3: Initial Enlargement Fraction" << endl;
    nestedSampler.outputFile << "# Row #4: Shrinking Rate" << endl;
    nestedSampler.outputFile << minNclusters << endl;
    nestedSampler.outputFile << maxNclusters << endl;
    nestedSampler.outputFile << initialEnlargementFraction << endl;
    nestedSampler.outputFile << shrinkingRate << endl;
    nestedSampler.outputFile.close();

    // Save the results in output files -----

    Results results(nestedSampler);
    results.writeParametersToFile("parameter");
    results.writeLogLikelihoodToFile("logLikelihood.txt");
    results.writeLogWeightsToFile("logWeights.txt");
    results.writeEvidenceInformationToFile("evidenceInformation.txt");
    results.writePosteriorProbabilityToFile("posteriorDistribution.txt");

    double credibleLevel = 68.3;
    bool writeMarginalDistributionToFile = true;
    results.writeParametersSummaryToFile("parameterSummary.txt", credibleLevel, writeMarginalDistributionToFile);

    cout << "Process completed." << endl;

    return EXIT_SUCCESS;
}
