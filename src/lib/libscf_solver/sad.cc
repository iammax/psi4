/*
 *  sad.cc
 *
 * Routines for the high-meantenance SAD guess
 * and daul-basis projections
 *
 */

#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <vector>
#include <utility>

#include <psifiles.h>
#include <libciomr/libciomr.h>
#include <libpsio/psio.h>
#include <libchkpt/chkpt.hpp>
#include <libparallel/parallel.h>
#include <libipv1/ip_lib.h>
#include <libiwl/iwl.hpp>
#include <libqt/qt.h>
#include <psifiles.h>

#include <libmints/mints.h>

#include "hf.h"
#include "rhf.h"

using namespace boost;
using namespace std;
using namespace psi;

namespace psi { namespace scf {

void HF::getUHFAtomicDensity(shared_ptr<BasisSet> bas, int nelec, int nhigh, double** D)
{
    int sad_print_ = options_.get_int("SAD_PRINT");
    //ONLY WORKS IN C1 at the moment
    //print_ = options_.get_int("PRINT");
    shared_ptr<Molecule> mol = bas->molecule();    
    
    int nbeta = (nelec-nhigh)/2;
    int nalpha = nelec-nbeta;
    int natom = mol->natom();
    int norbs = bas->nbf();
    
    if (sad_print_>1 && Communicator::world->me() == 0) {
        fprintf(outfile,"\n");
        bas->print(outfile);
        fprintf(outfile,"  Occupation: nalpha = %d, nbeta = %d, norbs = %d\n",nalpha,nbeta,norbs);
        fprintf(outfile,"\n  Atom:\n");
        mol->print();
    }

    if (natom != 1) {
        throw std::domain_error("SAD Atomic UHF has been given a molecule, not an atom"); 
    }

    //double xc = mol->x(0);
    //double yc = mol->y(0);
    //double zc = mol->z(0);

    //Vector3 vb = bas->shell(0)->center();
    //double xb = vb[0];
    //double yb = vb[1];
    //double zb = vb[2];

    //fprintf(outfile, " Molecule center <%14.10f,%14.10f,%14.10f>, Basis center <%14.10f,%14.10f,%14.10f>.\n", \
        xc, yc, zc, xb, yb, zb);

    double** Dold = block_matrix(norbs,norbs);
    double **Shalf = block_matrix(norbs, norbs);
    double** Ca = block_matrix(norbs,norbs);
    double** Cb = block_matrix(norbs,norbs);
    double** Da = block_matrix(norbs,norbs);
    double** Db = block_matrix(norbs,norbs);    
    double** Fa = block_matrix(norbs,norbs);
    double** Fb = block_matrix(norbs,norbs);
    double** Fa_old = block_matrix(norbs,norbs);
    double** Fb_old = block_matrix(norbs,norbs);    
    double** Ga = block_matrix(norbs,norbs);
    double** Gb = block_matrix(norbs,norbs);    

    IntegralFactory integral(bas, bas, bas, bas);
    MatrixFactory mat;
    mat.init_with(1,&norbs,&norbs);
    OneBodyInt *S_ints = integral.overlap();
    OneBodyInt *T_ints = integral.kinetic();
    OneBodyInt *V_ints = integral.potential();
    TwoBodyInt *TEI = integral.eri();

    //Compute Shalf;
    //Fill S
    SharedMatrix S_UHF(mat.create_matrix("S_UHF"));
    S_ints->compute(S_UHF);
    double** S = S_UHF->to_block_matrix();

    if (sad_print_>6 && Communicator::world->me() == 0) {
        fprintf(outfile,"  S:\n");
        print_mat(S,norbs,norbs,outfile);
    }
    // S^{-1/2}
    
    // First, diagonalize S
    // the C_DSYEV call replaces the original matrix J with its eigenvectors
    double* eigval = init_array(norbs);
    int lwork = norbs * 3;
    double* work = init_array(lwork);
    int stat = C_DSYEV('v','u',norbs,S[0],norbs,eigval, work,lwork);
    if (stat != 0) {
        if (Communicator::world->me() == 0)
            fprintf(outfile, "C_DSYEV failed\n");
        exit(PSI_RETURN_FAILURE);
    }
    free(work);
    
    double **S_copy = block_matrix(norbs, norbs);
    C_DCOPY(norbs*norbs,S[0],1,S_copy[0],1); 

    // Now form S^{-1/2} = U(T)*s^{-1/2}*U,
    // where s^{-1/2} is the diagonal matrix of the inverse square roots
    // of the eigenvalues, and U is the matrix of eigenvectors of S
    for (int i=0; i<norbs; i++) {
        if (eigval[i] < 1.0E-10)
            eigval[i] = 0.0;
        else 
            eigval[i] = 1.0 / sqrt(eigval[i]);

        // scale one set of eigenvectors by the diagonal elements s^{-1/2}
        C_DSCAL(norbs, eigval[i], S[i], 1);
    }
    free(eigval);

    // Smhalf = S_copy(T) * S
    C_DGEMM('t','n',norbs,norbs,norbs,1.0,
            S_copy[0],norbs,S[0],norbs,0.0,Shalf[0],norbs);

    free_block(S);
    free_block(S_copy);
    
    if (sad_print_>6 && Communicator::world->me() == 0) {
        fprintf(outfile,"  S^-1/2:\n");
        print_mat(Shalf,norbs,norbs,outfile);
    }

    //Compute H
    SharedMatrix T_UHF(mat.create_matrix("T_UHF"));
    T_ints->compute(T_UHF);
    SharedMatrix V_UHF(mat.create_matrix("V_UHF"));
    V_ints->compute(V_UHF);
    SharedMatrix H_UHF(mat.create_matrix("H_UHF"));
    H_UHF->zero();
    H_UHF->add(T_UHF);
    H_UHF->add(V_UHF);
    double** H = H_UHF->to_block_matrix();
   
    delete S_ints;
    delete T_ints;
    delete V_ints;
 
    if (sad_print_>6 && Communicator::world->me() == 0) {
        fprintf(outfile,"  H:\n");
        print_mat(H,norbs,norbs,outfile);
    }

    //Compute initial Ca and Da from core guess
    atomicUHFHelperFormCandD(nalpha,norbs,Shalf,H,Ca,Da);
    //Compute initial Cb and Db from core guess
    atomicUHFHelperFormCandD(nbeta,norbs,Shalf,H,Cb,Db);
    //Compute intial D  
    C_DCOPY(norbs*norbs,Da[0],1,D[0],1);
    C_DAXPY(norbs*norbs,1.0,Db[0],1,D[0],1);   
    if (sad_print_>6 && Communicator::world->me() == 0) {
        fprintf(outfile,"  Ca:\n");
        print_mat(Ca,norbs,norbs,outfile);

        fprintf(outfile,"  Cb:\n");
        print_mat(Cb,norbs,norbs,outfile);

        fprintf(outfile,"  Da:\n");
        print_mat(Da,norbs,norbs,outfile);

        fprintf(outfile,"  Db:\n");
        print_mat(Db,norbs,norbs,outfile);

        fprintf(outfile,"  D:\n");
        print_mat(D,norbs,norbs,outfile);
    }

    //Compute inital E for reference
    double E = C_DDOT(norbs*norbs,D[0],1,H[0],1); 
    E += C_DDOT(norbs*norbs,Da[0],1,Fa[0],1); 
    E += C_DDOT(norbs*norbs,Db[0],1,Fb[0],1); 
    E *= 0.5;       

    const double* buffer = TEI->buffer();

    double E_tol = options_.get_double("SAD_E_CONVERGE");
    double D_tol = options_.get_double("SAD_D_CONVERGE");
    int maxiter = options_.get_int("SAD_MAXITER");
    int f_mixing_iteration = options_.get_int("SAD_F_MIX_START");

    double E_old;
    int iteration = 0;

    bool converged = false; 
    if (sad_print_>1 && Communicator::world->me() == 0) {
        fprintf(outfile, "\n  Initial Atomic UHF Energy:    %14.10f\n\n",E);
        fprintf(outfile, "                                         Total Energy            Delta E              Density RMS\n\n");
        fflush(outfile);
    }
    do {

        iteration++;

        //Copy the old values over for error analysis 
        E_old = E;    
        //I'm only going to use the total for now, could be expanded later
        C_DCOPY(norbs*norbs,D[0],1,Dold[0],1);    
        //And old Fock matrices for level shift
        C_DCOPY(norbs*norbs,Fa[0],1,Fa_old[0],1);    
        C_DCOPY(norbs*norbs,Fb[0],1,Fb_old[0],1);    

        //Form Ga and Gb via integral direct
        memset((void*) Ga[0], '\0',norbs*norbs*sizeof(double));    
        memset((void*) Gb[0], '\0',norbs*norbs*sizeof(double));    
    
        //At the moment this is 8-fold slower than it could be, we'll see if it is signficant
        for (int MU = 0; MU < bas->nshell(); MU++) {
        int numMU = bas->shell(MU)->nfunction();
        for (int NU = 0; NU < bas->nshell(); NU++) {
        int numNU = bas->shell(NU)->nfunction();
        for (int LA = 0; LA < bas->nshell(); LA++) {
        int numLA = bas->shell(LA)->nfunction();
        for (int SI = 0; SI < bas->nshell(); SI++) {
        int numSI = bas->shell(SI)->nfunction();
        TEI->compute_shell(MU,NU,LA,SI);
        for (int m = 0, index = 0; m < numMU; m++) {
        int omu = bas->shell(MU)->function_index() + m;
        for (int n = 0; n < numNU; n++) {
        int onu = bas->shell(NU)->function_index() + n;
        for (int l = 0; l < numLA; l++) {
        int ola = bas->shell(LA)->function_index() + l;
        for (int s = 0; s < numSI; s++, index++) {
        int osi = bas->shell(SI)->function_index() + s;
             //fprintf(outfile,"  Integral (%d, %d| %d, %d) = %14.10f\n",omu,onu,ola,osi,buffer[index]);
             Ga[omu][onu] += D[ola][osi]*buffer[index];
             //Ga[ola][osi] += D[omu][onu]*buffer[index];
             Ga[omu][osi] -= Da[onu][ola]*buffer[index]; 
             Gb[omu][onu] += D[ola][osi]*buffer[index];
             //Gb[ola][osi] += D[omu][onu]*buffer[index];
             Gb[omu][osi] -= Db[onu][ola]*buffer[index]; 
        } 
        } 
        } 
        } 
        }      
        }      
        }      
        }      

        //Form Fa and Fb
        C_DCOPY(norbs*norbs,H[0],1,Fa[0],1);
        C_DAXPY(norbs*norbs,1.0,Ga[0],1,Fa[0],1);   
        C_DCOPY(norbs*norbs,H[0],1,Fb[0],1);
        C_DAXPY(norbs*norbs,1.0,Gb[0],1,Fb[0],1);   
    
        //Compute E
        E = C_DDOT(norbs*norbs,D[0],1,H[0],1); 
        E += C_DDOT(norbs*norbs,Da[0],1,Fa[0],1); 
        E += C_DDOT(norbs*norbs,Db[0],1,Fb[0],1); 
        E *= 0.5;       
 
        //Perform any required convergence stabilization
        //F-mixing (should help with oscillation)
        //20% old, 80% new Fock matrix for now
        if (iteration >= f_mixing_iteration) {
            C_DSCAL(norbs*norbs,0.8,Fa[0],1);
            C_DSCAL(norbs*norbs,0.8,Fb[0],1);
            C_DAXPY(norbs*norbs,0.2,Fa_old[0],1,Fa[0],1); 
            C_DAXPY(norbs*norbs,0.2,Fb_old[0],1,Fb[0],1); 
        }

        //Diagonalize Fa and Fb to from Ca and Cb and Da and Db
        atomicUHFHelperFormCandD(nalpha,norbs,Shalf,Fa,Ca,Da);
        atomicUHFHelperFormCandD(nbeta,norbs,Shalf,Fb,Cb,Db);

        //Form D
        C_DCOPY(norbs*norbs,Da[0],1,D[0],1);
        C_DAXPY(norbs*norbs,1.0,Db[0],1,D[0],1);  

        //Form delta D and Drms
        C_DAXPY(norbs*norbs,-1.0,D[0],1,Dold[0],1);
        double Drms = sqrt(1.0/(1.0*norbs*norbs)*C_DDOT(norbs*norbs,Dold[0],1,Dold[0],1));  

        double deltaE = fabs(E-E_old);        
    
        if (sad_print_>6 && Communicator::world->me() == 0) {
            fprintf(outfile,"  Fa:\n");
            print_mat(Fa,norbs,norbs,outfile);

            fprintf(outfile,"  Fb:\n");
            print_mat(Fb,norbs,norbs,outfile);

            fprintf(outfile,"  Ga:\n");
            print_mat(Ga,norbs,norbs,outfile);

            fprintf(outfile,"  Gb:\n");
            print_mat(Gb,norbs,norbs,outfile);

            fprintf(outfile,"  Ca:\n");
            print_mat(Ca,norbs,norbs,outfile);

            fprintf(outfile,"  Cb:\n");
            print_mat(Cb,norbs,norbs,outfile);

            fprintf(outfile,"  Da:\n");
            print_mat(Da,norbs,norbs,outfile);

            fprintf(outfile,"  Db:\n");
            print_mat(Db,norbs,norbs,outfile);

            fprintf(outfile,"  D:\n");
            print_mat(D,norbs,norbs,outfile);
        }
        if (sad_print_>1 && Communicator::world->me() == 0)
            fprintf(outfile, "  @Atomic UHF iteration %3d energy: %20.14f    %20.14f %20.14f\n", iteration, E, E-E_old, Drms);
        if (iteration > 1 && deltaE < E_tol && Drms < D_tol)
            converged = true;

        if (iteration > maxiter) {
            if(Communicator::world->me() == 0)
                fprintf(outfile, "  Atomic UHF is not converging! Try casting from a smaller basis or call Rob at CCMST.\n");
            break;
        }

        //Check convergence 
    } while (!converged);
    if (converged && print_ > 1 && Communicator::world->me() == 0)
        fprintf(outfile, "\n  @Atomic UHF Final Energy: %20.14f\n", E);
   
    delete TEI; 
    free_block(Dold);
    free_block(Ca);
    free_block(Cb);
    free_block(Da);
    free_block(Db);
    free_block(Fa);
    free_block(Fb);
    free_block(Fa_old);
    free_block(Fb_old);
    free_block(Ga);
    free_block(Gb);
    free_block(H);
    free_block(Shalf);
}
void HF::atomicUHFHelperFormCandD(int nelec, int norbs,double** Shalf, double**F, double** C, double** D)
{
    int sad_print_ = options_.get_int("SAD_PRINT");
    //Forms C in the AO basis for SAD Guesses
    double **Temp = block_matrix(norbs,norbs);
    double **Fp = block_matrix(norbs,norbs);
    double **Cp = block_matrix(norbs,norbs);
    
    //Form F' = X'FX = XFX for symmetric orthogonalization 
    C_DGEMM('N','N',norbs,norbs,norbs,1.0,Shalf[0],norbs,F[0],norbs,0.0,Temp[0],norbs);  
    C_DGEMM('N','N',norbs,norbs,norbs,1.0,Temp[0],norbs,Shalf[0],norbs,0.0,Fp[0],norbs);  

    //Form C' = eig(F')
    double *eigvals = init_array(norbs);
    sq_rsp(norbs, norbs, Fp,  eigvals, 1, Cp, 1.0e-14);
    free(eigvals);    

    //Form C = XC'
    C_DGEMM('N','N',norbs,norbs,norbs,1.0,Shalf[0],norbs,Cp[0],norbs,0.0,C[0],norbs);

    //Form D = Cocc*Cocc'
    C_DGEMM('N','T',norbs,norbs,nelec,1.0,C[0],norbs,C[0],norbs,0.0,D[0],norbs); 

    free_block(Temp);
    free_block(Cp);
    free_block(Fp);
}
void RHF::compute_SAD_guess()
{
    int sad_print_ = options_.get_int("SAD_PRINT");

    shared_ptr<Molecule> mol = basisset_->molecule();
    std::vector<shared_ptr<BasisSet> > atomic_bases;

    if (print_ > 6) {
        fprintf(outfile,"\n  Constructing atomic basis sets\n  Molecule:\n");
        mol->print();
    }


    //Build the atomic basis sets for libmints use in UHF
    for (int A = 0; A<mol->natom(); A++) {
        atomic_bases.push_back(basisset_->atomic_basis_set(A));
        if (sad_print_>6) {
            fprintf(outfile,"  SAD: Atomic Basis Set %d\n", A);
            atomic_bases[A]->molecule()->print();
            fprintf(outfile,"\n");
            atomic_bases[A]->print(outfile);
            fprintf(outfile,"\n");
        }
    }

    //Spin occupations per atom, to be determined by Hund's Rules
    //or user input
    int* nalpha = init_int_array(mol->natom());
    int* nbeta = init_int_array(mol->natom());
    int* nelec = init_int_array(mol->natom());
    int* nhigh = init_int_array(mol->natom());
    int tot_elec = 0;

    //Ground state high spin occupency array, atoms 0 to 36 (see Giffith's Quantum Mechanics, pp. 217)
    const int reference_S[] = {0,1,0,1,0,1,2,3,2,1,0,1,0,1,2,3,2,1,0,1,0,1,2,3,6,5,4,3,2,1,0,1,2,3,2,1,0};
    const int MAX_Z = 36;

    //At the moment, we'll assume no ions, and Hund filling
    //Improvemnts to SAD can be made simply by changing the setup of nalpha and nbeta
    if (sad_print_>1)
        fprintf(outfile,"\n  SAD: Determining atomic occupations:\n");
    for (int A = 0; A<mol->natom(); A++) {
        int Z = mol->Z(A);
        if (Z>MAX_Z) {
            throw std::domain_error(" Only Atoms up to 36 (Kr) are currently supported with SAD Guess");
        }
        nhigh[A] = reference_S[Z];
        nelec[A] = Z;
        tot_elec+= nelec[A];
        nbeta[A] = (nelec[A]-nhigh[A])/2;
        nalpha[A] = nelec[A]-nbeta[A];
        if (sad_print_>1)
            fprintf(outfile,"  Atom %d, Z = %d, nelec = %d, nhigh = %d, nalpha = %d, nbeta = %d\n",A,Z,nelec[A],nhigh[A],nalpha[A],nbeta[A]);
    }
    fflush(outfile);

    // Determine redundant atoms
    int* unique_indices = init_int_array(mol->natom()); // All atoms to representative unique atom
    int* atomic_indices = init_int_array(mol->natom()); // unique atom to first representative atom 
    int* offset_indices = init_int_array(mol->natom()); // unique atom index to rank 
    int nunique = 0;
    for (int l = 0; l < mol->natom(); l++) {
        unique_indices[l] = l;
        atomic_indices[l] = l;
    }
    for (int l = 0; l < mol->natom() - 1; l++) {
        for (int m = l + 1; m < mol->natom(); m++) {
            if (unique_indices[m] != m)
                continue; //Already assigned
            if (mol->Z(l) != mol->Z(m))
                continue;
            if (nalpha[l] != nalpha[m])
                continue;
            if (nbeta[l] != nbeta[m])
                continue;
            if (nhigh[l] != nhigh[m])
                continue;
            if (nelec[l] != nelec[m])
                continue;
            if (atomic_bases[l]->nbf() != atomic_bases[m]->nbf())
                continue;
            if (atomic_bases[l]->nshell() != atomic_bases[m]->nshell())
                continue;
            if (atomic_bases[l]->nprimitive() != atomic_bases[m]->nprimitive())
                continue;
            if (atomic_bases[l]->max_am() != atomic_bases[m]->max_am())
                continue;
            if (atomic_bases[l]->max_nprimitive() != atomic_bases[m]->max_nprimitive())
                continue;
            if (atomic_bases[l]->has_puream() !=  atomic_bases[m]->has_puream())
                continue;

            //TODO check the basis rigorously            
            
            //Rigorous match obtained
            unique_indices[m] = l;
        }
    }
    for (int l = 0; l < mol->natom(); l++) {
        if (unique_indices[l] == l) {
            atomic_indices[nunique] = l;
            offset_indices[l] = nunique;
            nunique++;
        }    
    }

    //printf(" Nunique = %d \n", nunique);
    //printf(" Unique Indices   Atomic Indices \n");
    //for (int l = 0; l < mol->natom(); l++) {
    //    printf(" %14d   %14d \n", unique_indices[l], atomic_indices[l]);
    //}    

    timer_on("Atomic UHF");

    //Atomic D matrices within the atom specific AO basis
    double*** atomic_D = (double***)malloc(nunique*sizeof(double**));
    for (int A = 0; A<nunique; A++) {
        atomic_D[A] = block_matrix(atomic_bases[atomic_indices[A]]->nbf(),atomic_bases[atomic_indices[A]]->nbf());
    }

    if (sad_print_>1)
        fprintf(outfile,"\n  Performing Atomic UHF Computations:\n");
    for (int A = 0; A<nunique; A++) {
        int index = atomic_indices[A];
        if (sad_print_>1)
            fprintf(outfile,"\n  UHF Computation for Unique Atom %d which is Atom %d:\n",A, index);
        getUHFAtomicDensity(atomic_bases[index],nelec[index],nhigh[index],atomic_D[A]);
    }
    timer_off("Atomic UHF");

    //Add atomic_D into D (scale by 1/2, we like pairs in RHF)
    D_->zero();
    for (int A = 0, offset = 0; A < mol->natom(); A++) {
        int norbs = atomic_bases[A]->nbf();
        int back_index = unique_indices[A];
        for (int m = 0; m<norbs; m++)
            for (int n = 0; n<norbs; n++)
                D_->set(0,m+offset,n+offset,0.5*atomic_D[offset_indices[back_index]][m][n]);
        offset+=norbs;
    }
    if (sad_print_>6)
        D_->print(outfile);

    free(atomic_indices);
    free(unique_indices);
    free(offset_indices);
    //D_->print(outfile);

    //A C matrix is needed. Do one of:
    //   --An integral direct step (expensive)
    //   --A Cholesky orbital approximation (cheap, should be preferred after the David Sherrill correction)
    if (options_.get_str("SAD_C") == "ID") {
        //compute a rough fock matrix via integral direct
        //note, my convention is backwards, l and s are index variables
        // m and n are zip variables
        timer_on("sad fock");
        double sad_schwarz = options_.get_double("sad_schwarz_cutoff");
        G_->zero();
        IntegralFactory integral(basisset_, basisset_, basisset_, basisset_);
        TwoBodyInt *tei = integral.eri(0, sad_schwarz);
        const double* buffer = tei->buffer();
        for (int a = 0, shell_offset = 0; a<mol->natom(); a++) {
        for (int mu = shell_offset; mu < shell_offset+atomic_bases[a]->nshell(); mu++) {
        int nummu = basisset_->shell(mu)->nfunction();
        for (int nu = shell_offset; nu < shell_offset+atomic_bases[a]->nshell() ; nu++) {
        int numnu = basisset_->shell(nu)->nfunction();
        for (int la = 0; la < basisset_->nshell(); la++) {
        int numla = basisset_->shell(la)->nfunction();
        for (int si = 0; si < basisset_->nshell(); si++) {
        int numsi = basisset_->shell(si)->nfunction();
    
        //j
        if (!tei->shell_is_zero(mu,nu,la,si)) {
        tei->compute_shell(mu,nu,la,si);
        for (int m = 0, index = 0; m<nummu; m++) {
        int omu = basisset_->shell(mu)->function_index() + m;
        for (int n = 0; n<numnu; n++) {
        int onu = basisset_->shell(nu)->function_index() + n;
        for (int l = 0; l<numla; l++) {
        int ola = basisset_->shell(la)->function_index() + l;
        for (int s = 0; s<numsi; s++,index++) {
        int osi = basisset_->shell(si)->function_index() + s;
            G_->add(0,ola,osi,2.0*D_->get(0,omu,onu)*buffer[index]);
        }
        }
        }
        }
        }
    
        //k
        if (!tei->shell_is_zero(la,mu,nu,si)) {
        tei->compute_shell(la,mu,nu,si);
        for (int l = 0, index = 0; l<numla; l++) {
        int ola = basisset_->shell(la)->function_index() + l;
        for (int m = 0; m<nummu; m++) {
        int omu = basisset_->shell(mu)->function_index() + m;
        for (int n = 0; n<numnu; n++) {
        int onu = basisset_->shell(nu)->function_index() + n;
        for (int s = 0; s<numsi; s++,index++) {
        int osi = basisset_->shell(si)->function_index() + s;
            G_->add(0,ola,osi,-D_->get(0,omu,onu)*buffer[index]);
        }
        }
        }
        }
        }
        //fprintf(outfile,"  cholesky tensor: ntri_ = %d, ri_nbf_ = %d:\n",ntri_,ri_nbf_);
    
        }
        }
        }
        }
        shell_offset+= atomic_bases[a]->nshell();
        }
    
        F_->copy(H_);
        F_->add(G_);
    
        //compute initial e for reference
        E_ = compute_E();
    
        //form the c matrix from the rough fock matrix
        form_C();
        //form the d matrix from the resultant c matrix
        form_D();
        timer_off("sad fock");
    
        if (sad_print_>7) {
            G_->print(outfile);
            F_->print(outfile);
            C_->print(outfile);
            D_->print(outfile);
        }
    } else if (options_.get_str("SAD_C") == "CHOLESKY") {
        timer_on("SAD Cholesky");
        if (sad_print_) {
            fprintf(outfile,"  Approximating occupied orbitals via Partial Cholesky Decomposition.\n");
            fprintf(outfile,"  NOTE: The zero-th SCF iteration will not be variational.\n");
        }
        int norbs = nso_;
        int ndocc = nalpha_;
        sad_nocc_ = 0;

        double** D = D_->to_block_matrix();
        double* Temp = init_array(norbs);
        int* P = init_int_array(norbs);
        for (int i = 0; i<norbs; i++)
            P[i] = i;

        //fprintf(outfile,"  D:\n");
        //print_mat(D,norbs,norbs,outfile);
        //Pivot
        double max;
        int Temp_p;
        int pivot;
        for (int i = 0; i<norbs-1; i++) {
            max = 0.0;
            //Where's the pivot diagonal?
            for (int j = i; j<norbs; j++)
                if (max <= fabs(D[j][j])) {
                    max = fabs(D[j][j]);
                    pivot = j;
                }

            //Rows
            C_DCOPY(norbs,&D[pivot][0],1,Temp,1);
            C_DCOPY(norbs,&D[i][0],1,&D[pivot][0],1);
            C_DCOPY(norbs,Temp,1,&D[i][0],1);

            //Columns
            C_DCOPY(norbs,&D[0][pivot],norbs,Temp,1);
            C_DCOPY(norbs,&D[0][i],norbs,&D[0][pivot],norbs);
            C_DCOPY(norbs,Temp,1,&D[0][i],norbs);

            Temp_p = P[i];
            P[i] = P[pivot];
            P[pivot] = Temp_p;
        }

        //fprintf(outfile,"  D (pivoted):\n");
        //print_mat(D,norbs,norbs,outfile);
        //for (int i = 0; i<norbs; i++)
        //    fprintf(outfile,"  Pivot %d is %d.\n",i,P[i]);

        //Cholesky Decomposition
        int rank = C_DPOTRF('U',norbs,D[0],norbs);
        if (rank < 0) {
            fprintf(outfile,"  Cholesky Decomposition Failed");
            fflush(outfile);
            exit(PSI_RETURN_FAILURE);
        }
        else if (rank == 0) {
            //Full Cholesky completed, use the whole thing
            rank = norbs;
            sad_nocc_ = norbs;
        } else {
            // Only a partial Cholesky completed (rank was deficient)
            sad_nocc_ = rank - 1; //FORTRAN 
        }
        for (int i = 0; i<norbs-1; i++)
            for (int j = i+1; j<norbs; j++)
                D[i][j] = 0.0;

        //fprintf(outfile,"  C Guess (Cholesky Unpivoted) , rank is %d:\n", rank);
        //print_mat(D,norbs,sad_nocc_,outfile);

        //Unpivot
        double** C = block_matrix(norbs,sad_nocc_);
        for (int m = 0; m < norbs; m++) {
            C_DCOPY(sad_nocc_,&D[m][0],1,&C[P[m]][0],1);
        }

        //Eliminate the redundancies
        double chol_cutoff = options_.get_double("SAD_CHOL_CUTOFF");
        for (int i = ndocc; i<sad_nocc_; i++) {
            if (sqrt(1.0/(norbs)*C_DDOT(norbs,&C[0][i],sad_nocc_,&C[0][i],sad_nocc_))<chol_cutoff)
                sad_nocc_--;
        }

        if (sad_print_>1)
            fprintf(outfile,"  %d of %d possible partial occupation vectors selected for C.\n",sad_nocc_,norbs);

        //Set C
        C_->zero();
        for (int m = 0; m<norbs; m++)
            for (int i = 0; i<sad_nocc_; i++)
                C_->set(0,m,i,C[m][i]);
        //fprintf(outfile,"  C Guess (Cholesky):\n");
        //print_mat(C,norbs,sad_nocc_,outfile);

        //C_->print(outfile);

        free(P);
        free(Temp);
        free_block(D);
        free_block(C);

        E_ = 0.0; //For now
        doccpi_[0] = nalpha_;//Also for now

        timer_off("SAD Cholesky");

    } else {
        throw std::invalid_argument("ID or CHOLESKY are the only two C matrix generators at the moment");
    }
    for (int A = 0; A<nunique; A++)
        free_block(atomic_D[A]);
    free(atomic_D);

    free(nelec);
    free(nhigh);
    free(nalpha);
    free(nbeta);
    //abort();
}
SharedMatrix HF::dualBasisProjection(SharedMatrix C_, int nocc, shared_ptr<BasisSet> old_basis, shared_ptr<BasisSet> new_basis) {
    
    //Based on Werner's method from Mol. Phys. 102, 21-22, 2311

    //ALSO ONLY C1 at the moment
    //The old C matrix
    double** CA = C_->to_block_matrix();

    //sizes
    int old_norbs = old_basis->nbf();
    int new_norbs = new_basis->nbf();        

    //fprintf(outfile,"  Old norbs %d, New norbs %d, nocc %d\n",old_norbs,new_norbs,nocc);

    //AB Integral facotry (acts on first two elements)
    IntegralFactory integralAB(old_basis, new_basis, old_basis, new_basis);
    MatrixFactory matAB;
    matAB.init_with(1,&old_norbs,&new_norbs);
    
    //Overlap integrals (basisset to basisset)
    OneBodyInt *SAB_ints = integralAB.overlap();
    SharedMatrix S_AB(matAB.create_matrix("S_AB"));
    SAB_ints->compute(S_AB);
    //S_AB->print(outfile);
    double** SAB = S_AB->to_block_matrix();

    //BB Integral facotry (acts on first two elements)
    IntegralFactory integralBB(new_basis, new_basis, new_basis, new_basis);
    MatrixFactory matBB;
    matBB.init_with(1,&new_norbs,&new_norbs);
    
    //Overlap integrals (basisset to basisset)
    OneBodyInt *SBB_ints = integralBB.overlap();
    SharedMatrix S_BB(matBB.create_matrix("S_BB"));
    SBB_ints->compute(S_BB);
    //S_BB->print(outfile);
    double** SBB = S_BB->to_block_matrix();
    
    //Inverse overlap matrices
    //double** SAAM1 = block_matrix(old_norbs,old_norbs);
    double** SBBM1 = block_matrix(new_norbs,new_norbs);

    //Copy the values in
    C_DCOPY(new_norbs*new_norbs,SBB[0],1,SBBM1[0],1);

    //SBB^-1
    int CholError = C_DPOTRF('L',new_norbs,SBBM1[0],new_norbs);
    if (CholError !=0 )
        throw std::domain_error("S_BB Matrix Cholesky failed!");
    
    //Inversion (in place)
    int IError = C_DPOTRI('L',new_norbs,SBBM1[0],new_norbs);
    if (IError !=0 )
        throw std::domain_error("S_BB Inversion Failed!");

    //LAPACK is smart and all, only uses half of the thing
    for (int m = 0; m<new_norbs; m++)
        for (int n = 0; n<m; n++)
            SBBM1[m][n] = SBBM1[n][m]; 

    //fprintf(outfile,"  S_BB^-1:\n");
    //print_mat(SBBM1,new_norbs,new_norbs,outfile);     
 
    //Form T 
    double** Temp1 = block_matrix(new_norbs,nocc);
    C_DGEMM('T','N',new_norbs,nocc,old_norbs,1.0,SAB[0],new_norbs,CA[0],old_norbs,0.0,Temp1[0],nocc);

    //fprintf(outfile," Temp1:\n");
    //print_mat(Temp1,new_norbs,nocc,outfile);     
    
    double** Temp2 = block_matrix(new_norbs,nocc);  
    C_DGEMM('N','N',new_norbs,nocc,new_norbs,1.0,SBBM1[0],new_norbs,Temp1[0],nocc,0.0,Temp2[0],nocc);

    //fprintf(outfile," Temp2:\n");
    //print_mat(Temp2,new_norbs,nocc,outfile);     
    
    double** Temp3 = block_matrix(old_norbs,nocc);
    C_DGEMM('N','N',old_norbs,nocc,new_norbs,1.0,SAB[0],new_norbs,Temp2[0],nocc,0.0,Temp3[0],nocc);
    
    //fprintf(outfile," Temp3:\n");
    //print_mat(Temp3,old_norbs,nocc,outfile);     
    
    double** T = block_matrix(nocc,nocc);
    C_DGEMM('T','N',nocc,nocc,old_norbs,1.0,CA[0],old_norbs,Temp3[0],nocc,0.0,T[0],nocc);
    
    //fprintf(outfile," T:\n");
    //print_mat(T,nocc,nocc,outfile);     
    
    //Find T^-1/2
    // First, diagonalize T
    // the C_DSYEV call replaces the original matrix T with its eigenvectors
    double* eigval = init_array(nocc);
    int lwork = nocc * 3;
    double* work = init_array(lwork);
    int stat = C_DSYEV('v','u',nocc,T[0],nocc,eigval, work,lwork);
    if (stat != 0) {
        if(Communicator::world->me() == 0)
            fprintf(outfile, "C_DSYEV failed\n");
        exit(PSI_RETURN_FAILURE);
    }
    free(work);

    // Now T contains the eigenvectors of the original T
    // Copy T to T_copy
    double **T_mhalf = block_matrix(nocc, nocc);
    double **T_copy = block_matrix(nocc, nocc);
    C_DCOPY(nocc*nocc,T[0],1,T_copy[0],1); 

    // Now form T^{-1/2} = U(T)*t^{-1/2}*U,
    // where t^{-1/2} is the diagonal matrix of the inverse square roots
    // of the eigenvalues, and U is the matrix of eigenvectors of T
    for (int i=0; i<nocc; i++) {
        if (eigval[i] < 1.0E-10)
            eigval[i] = 0.0;
        else 
            eigval[i] = 1.0 / sqrt(eigval[i]);

        // scale one set of eigenvectors by the diagonal elements t^{-1/2}
        C_DSCAL(nocc, eigval[i], T[i], 1);
    }
    free(eigval);

    // T_mhalf = T_copy(T) * T
    C_DGEMM('t','n',nocc,nocc,nocc,1.0,
            T_copy[0],nocc,T[0],nocc,0.0,T_mhalf[0],nocc);

    //Form CB
    double** CB = block_matrix(new_norbs,nocc);
    C_DGEMM('N','N',new_norbs,nocc,nocc,1.0,Temp2[0],nocc,T_mhalf[0],nocc,0.0,CB[0],nocc);
    
    //fprintf(outfile," T^-1/2:\n");
    //print_mat(T_mhalf,nocc,nocc,outfile);     
    
    //Set occupied part of C_B; 
    MatrixFactory matBI;
    matBI.init_with(1,&new_norbs,&nocc);
    
    SharedMatrix C_B(matBI.create_matrix("C_DB"));
    for (int m = 0; m < new_norbs; m++) 
        for (int i = 0; i<nocc; i++)
            C_B->set(0,m,i,CB[m][i]); 

    free_block(CA);   
    free_block(SAB);   
    free_block(SBB);   
    free_block(SBBM1);   
    free_block(Temp1);   
    free_block(Temp2);   
    free_block(Temp3);   
    free_block(T);   
    free_block(T_mhalf);   
    free_block(T_copy);   
    free_block(CB);   
 
    return C_B;
}

}}