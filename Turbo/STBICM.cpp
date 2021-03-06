/** \file
 * 
 * \brief Space Time Bit Interleaved Coded Modulation (ST BICM)
 *
 * Implements ST BICM using a turbo receiver with a SISO demapper module and a SISO NSC module.
 *
 * Reference: B. Cristea, ''Turbo receivers for Space-Time BICM``, to be published in IEEE Transactions on Wireless Communications
 */

#define TO_FILE

#include "itpp/itcomm.h"
#include "SISO.h"//SISO class
#include "STC.h" //STC class
#include "Progress_Timer.h"

using namespace itpp;
using tr::STC;
using tr::SISO;
using tr::threshold;
using std::cout;
using std::endl;
using std::string;

int main(void)
{
    //receiver parameters
    ivec gen = "0133 0171";
    int constraint_length = 7;
    int const_size = 4;//constellation size
    int coherence_time = 512;//expressed in symbol durations, multiple of T, T<=coherence_time<=tx_duration, T is the ST code duration
    double threshold_value = 50;
    string map_metric="maxlogMAP";
    string demapper_method = "Hassibi_maxlogMAP";//Hassibi_maxlogMAP or GA or sGA or mmsePIC or zfPIC or Alamouti_maxlogMAP
    int nb_errors_lim = 1500;
    int nb_bits_lim = int(1e6);
    int perm_len = pow2i(14);//permutation length
    int nb_iter = 5;//number of iterations in the turbo decoder
    int rec_antennas = 2;//number of reception antennas
    vec EbN0_dB = "0:20";
    double Es = 1.0;//mean symbol energy

    //convolutional code generator polynomials
    Convolutional_Code nsc;
    nsc.set_generator_polynomials(gen, constraint_length);
    double coding_rate = 1.0/2.0;

    //QAM modulator class
    QAM mod(const_size);

    //Space-Time code parameters
    int em_antennas = 2;//number of emission antennas
    int channel_uses = 2;//ST code duration
    string code_name = "Golden_2x2";//V-BLAST_MxN, Golden_2x2, Damen_2x2, Alamouti_2xN    
    STC st_block_code;
    st_block_code.setup(em_antennas, channel_uses, code_name, const_size);//generate matrices for LD code (following Hassibi's approach)
    int symb_block = st_block_code.get_nb_symbols_per_block();
    em_antennas = st_block_code.get_nb_em_antennas();//these parameters could by changed depending on the selected code
    channel_uses = st_block_code.get_channel_uses();    

    //recompute interleaver length
    int G = coherence_time*mod.bits_per_symbol()*symb_block;
    perm_len = G*(perm_len/G);//recompute interleaver length
    int block_len = int(coding_rate*perm_len);//informational block length
    int nb_symb = perm_len/mod.bits_per_symbol();//number of symbols at the modulator output
    int nb_subblocks = nb_symb/symb_block;//number of blocks of ST code emitted in an interleaver period
    int tx_duration = channel_uses*nb_subblocks;//transmission duration expressed in number of symbol periods

    //fading channel parameters
    if (coherence_time%channel_uses)//check if the coherence time is a multiple of channel_uses
    {
        coherence_time = channel_uses*(coherence_time/channel_uses);
        std::cout << "Warning! The coherence time must be a multiple of T. Choosing coherence_time=channel_uses*floor(coherence_time/channel_uses) = "\
        << coherence_time << std::endl;
    }
    if (coherence_time>tx_duration)
    {
        coherence_time = channel_uses*(tx_duration/channel_uses);
        std::cout << "Warning! The coherence time must be <= tx_duration. Choosing coherence_time = channel_uses*floor(tx_duration/channel_uses) = "\
        << coherence_time << std::endl;
    }
    cmat fading_pattern = ones_c(1, coherence_time/channel_uses);

    //other parameters
    string filename = "Res/STBICM_"+map_metric+"_"+demapper_method+".it";
    double R = coding_rate*double(mod.bits_per_symbol()*symb_block)/double(channel_uses);//ST code rate in (info.) bits/channel use
    vec sigma2 = (0.5*Es/(R*double(mod.bits_per_symbol())))*pow(inv_dB(EbN0_dB), -1.0);//N0/2
    int nb_blocks;//number of blocks
    int nb_errors;
    bvec bits(block_len);//data bits
    bvec coded_bits(perm_len);//no tail
    cvec em(nb_symb);
    ivec perm(perm_len);
    ivec inv_perm(perm_len);
    //SISO demapper
    vec demapper_apriori_data(perm_len);
    vec demapper_extrinsic_data(perm_len);
    //SISO NSC
    vec nsc_intrinsic_coded(perm_len);
    vec nsc_apriori_data(block_len);
    nsc_apriori_data.zeros();//always zero
    vec nsc_extrinsic_coded(perm_len);
    vec nsc_extrinsic_data(block_len);
    //decision
    bvec rec_bits(block_len);    
    int snr_len = EbN0_dB.length();
    mat ber(nb_iter,snr_len);
    ber.zeros();
    register int en,n,ns;
    cmat S(tx_duration, em_antennas);
    cmat rec(tx_duration,rec_antennas);

    //Rayleigh fading
    cmat ch_attenuations(em_antennas*rec_antennas,tx_duration/channel_uses);

    //SISO blocks
    SISO siso;
    siso.set_map_metric(map_metric);
    siso.set_generators(gen, constraint_length);
    siso.set_demapper_method(demapper_method);
    siso.set_constellation(mod.bits_per_symbol(), mod.get_symbols(), mod.get_bits2symbols());      
    siso.set_st_block_code(st_block_code.get_nb_symbols_per_block(), st_block_code.get_1st_gen_matrix(), st_block_code.get_2nd_gen_matrix(), rec_antennas);
    
    //decision
    BPSK bpsk;
    
    //BER
    BERC berc;

    //progress timer
    tr::Progress_Timer timer;
    timer.set_max(snr_len);
        
    //Randomize generators
    RNG_randomize();
    
    //main loop
    timer.progress(0.0);
    for (en=0;en<snr_len;en++)
    {
        siso.set_noise(sigma2(en));
        nb_errors = 0;
        nb_blocks = 0;
        while ((nb_errors<nb_errors_lim) && (nb_blocks*block_len<nb_bits_lim))//if at the last iteration the nb. of errors is inferior to lim, then process another block
        {
            //permutation
            perm = sort_index(randu(perm_len));
            //inverse permutation
            inv_perm = sort_index(perm);

            //bits generation
            bits = randb(block_len);

            //convolutional code
            nsc.encode(bits, coded_bits);//no tail

            //permutation+QAM modulation
            em = mod.modulate_bits(coded_bits(perm))/sqrt(em_antennas);//normalize emitted symbols           

            //ST code
            S = st_block_code.encode(em);        

            /* channel matrices (there are tx_duration/tau_c different channel matrices MxN)
             * a channel matrix is represented as a M*Nx1 vector (first M elements are the first column of the channel matrix)
             * the channel matrix is constant over tau_c symbol periods (a multiple of T symbol durations)
             * the channel matrix is the transpose of the true channel matrix
             */
            ch_attenuations = kron(randn_c(em_antennas*rec_antennas, tx_duration/coherence_time), fading_pattern);

            //flat-fading MIMO channel
            for (ns=0;ns<nb_subblocks;ns++)
            	rec.set_submatrix(ns*channel_uses, (ns+1)*channel_uses-1, 0, rec_antennas-1, \
            		S(ns*channel_uses, (ns+1)*channel_uses-1, 0, em_antennas-1)*reshape(ch_attenuations.get_col(ns), em_antennas, rec_antennas));           		   		            
            rec += sqrt(2*sigma2(en))*randn_c(tx_duration,rec_antennas);//sigma2 is the variance on each dimension

            //turbo receiver
            demapper_apriori_data.zeros();//a priori information of emitted bits
            siso.set_impulse_response(ch_attenuations);
            for (n=0;n<nb_iter;n++)
            {           	
                //first decoder
                siso.demapper(demapper_extrinsic_data, rec, demapper_apriori_data);

                //deinterleave+threshold
                nsc_intrinsic_coded = threshold(demapper_extrinsic_data(inv_perm), threshold_value);

                //second decoder
                siso.nsc(nsc_extrinsic_coded, nsc_extrinsic_data, nsc_intrinsic_coded, nsc_apriori_data, false);

                //decision
                rec_bits = bpsk.demodulate_bits(-nsc_extrinsic_data);//suppose that a priori info is zero
                //count errors
                berc.clear();
                berc.count(bits, rec_bits);
                ber(n,en) += berc.get_errorrate();

                //interleave
                demapper_apriori_data = nsc_extrinsic_coded(perm);
            }//end iterations
            nb_errors += int(berc.get_errors());//get number of errors at the last iteration
            nb_blocks++;
        }//end blocks (while loop)

        //compute BER over all tx blocks
        ber.set_col(en, ber.get_col(en)/nb_blocks);

        //show progress
        timer.progress(1+en);
    }
    timer.toc_print();

#ifdef TO_FILE
    //save results to file
    it_file ff(filename);
    ff << Name("BER") << ber;
    ff << Name("EbN0_dB") << EbN0_dB;
    ff << Name("gen") << gen;
    ff << Name("coding_rate") << coding_rate;
    ff << Name("nb_iter") << nb_iter;
    ff << Name("block_len") << block_len;
    ff << Name("nb_errors_lim") << nb_errors_lim;
    ff << Name("nb_bits_lim") << nb_bits_lim;
    ff.close();
#else
    //show BER
    cout << ber << endl;
#endif

    return 0;
}

