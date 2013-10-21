FORWARD=$2
if [ ${#FORWARD} -eq 0 ] || [ ! -s $FORWARD ]; then FORWARD=data_cliques_paired_R1.fastq; fi
BACKWARD=$3
if [ ${#BACKWARD} -eq 0 ] || [ ! -s $BACKWARD ]; then BACKWARD=data_cliques_paired_R2.fastq; fi
SINGLE=$4
if [ ${#SINGLE} -eq 0 ] || [ ! -s $SINGLE ]; then SINGLE=data_cliques_single.fastq; fi

function alignMemPaired () {
    bwa index -a bwtsw $2  
    bwa mem -t 79 $2 $1 $3 > aln.sam 
    samtools faidx $2  
    samtools view -q 20 -F 4 -bt $2.fai aln.sam > aln.bam  
    samtools sort aln.bam reads  
    rm aln*  
    samtools view -uf 1 reads.bam > paired.bam 
    samtools view -uF 1 reads.bam > single_1.bam 
    rm reads.bam 
}
function alignMemSingle () {
    bwa index -a bwtsw $2  
    bwa mem -t 79 $2 $1 > aln.sam 
    samtools faidx $2 
    samtools view -q 20 -F 4 -bt $2.fai aln.sam > aln.bam 
    samtools sort aln.bam single_2 
    rm aln* 
}
alignMemPaired $FORWARD $1 $BACKWARD

if [ -s $SINGLE ]; then
    alignMemSingle $SINGLE $1
    samtools merge single.bam single_1.bam single_2.bam 
else
    mv single_1.bam single.bam
fi

samtools merge reads_unsorted.bam single.bam paired.bam 
samtools sort reads_unsorted.bam assembly 
samtools index assembly.bam 

java -jar $SAF/SequenceAlignmentFixer.jar -i assembly.bam

rm -rf assembly*

samtools sort -n single.bam single_sort 
samtools view single_sort.bam > single_sort.sam 
samtools view paired.bam > paired.sam 

touch paired.priors single.priors
if [ -s paired.sam ]; then
    bam-to-alignment-priors $1 paired.bam > paired.priors 
fi
if [ -s single_sort.sam ]; then
    bam-to-alignment-priors --unsorted --single-end $1 single_sort.bam > single.priors  
fi
if [ -s data_clique_to_reads.tsv ]; then
    cat paired.priors single.priors  | sort -k6,6 -g | python $HAPLOCLIQUE/scripts/manipulate_alignment_prior.py data_clique_to_reads.tsv > alignment.prior
else
    cat paired.priors single.priors  | sort -k6,6 -g > alignment.prior
fi
rm *sam *bam *.priors

if [ -s data_clique_to_reads.tsv ]; then
    time clever-core -c 2000 -s 1 -q 0.99 -o 0.6 -a < alignment.prior > data_clique.fastq;
else
    time clever-core -c 2000 -s 5 -q 0.95 -o 0.6 -a < alignment.prior > data_clique.fastq;
fi 
python $HAPLOCLIQUE/scripts/haploclique-postprocess.py < data_clique.fastq
rm data_clique.fastq

sed -i -e '/^$/d;s/@Clique1/@Clique/g' data_cliques_paired_R1.fastq
sed -i -e '/^$/d;s/@Clique2/@Clique/g' data_cliques_paired_R2.fastq
awk 'BEGIN {FS=""} {if (NR%4 == 2) { for (i = NF; i >= 1; i = i - 1) { if ($i == "A") { printf "T"; } else if ($i == "C") { printf "G";} else if ($i == "G") { printf "C";} else if ($i == "T") { printf "A";} else if ($i == "N") { printf "N";} } print ""; } else print $0 }' data_cliques_paired_R2.fastq > tmp.fastq
rm data_cliques_paired_R2.fastq
mv tmp.fastq data_cliques_paired_R2.fastq
sed -i -e '/^$/d;s/@Clique1/@Clique/g' data_cliques_single.fastq
rm -rf *-e