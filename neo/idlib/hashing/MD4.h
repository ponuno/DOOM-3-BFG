#ifndef __MD4_H__
#define __MD4_H__

/*
===============================================================================

	Calculates a checksum for a block of data
	using the MD4 message-digest algorithm.

===============================================================================
*/

unsigned long MD4_BlockChecksum( const void *data, int length );

/*
	The same checksum fed in pieces, for data that is not in one block (the bytes fed in
	order give the value MD4_BlockChecksum gives for them in one block).
*/
class idMD4Checksum {
public:
					idMD4Checksum();
	void			Update( const void * data, int length );
	unsigned long	Final();
private:
	unsigned long	state[4];
	unsigned long	count[2];
	unsigned char	buffer[64];
};

#endif /* !__MD4_H__ */
